import os
import json

from Components.ActionMap import ActionMap
from Components.ConfigList import ConfigListScreen
from Components.Console import Console
from Components.config import config, ConfigSubsection, ConfigSelection, ConfigBoolean, \
    getConfigListEntry, ConfigSubDict, ConfigInteger, ConfigNothing
from Components.Label import Label
from Components.Sources.StaticText import StaticText
from Plugins.Plugin import PluginDescriptor
from Screens.InfoBar import InfoBar, MoviePlayer
from Screens.MessageBox import MessageBox
from Screens.Screen import Screen
from Tools.BoundFunction import boundFunction
from enigma import eEnv, eServiceReference

import serviceapp_client

# On this VTi image, which reference types are eligible for recording is not
# a monkey-patch on the raw eServiceReference class (unlike upstream
# OpenPLi-style ServiceReference.py) - it's a plain instance method on the
# ServiceReference wrapper class itself (confirmed by decompiling the
# installed ServiceReference.pyo and RecordTimer.so's callers, TimerEntry.pyo
# and TimerSanityCheck.pyo, which all call serviceref.isRecordable() on a
# ServiceReference-wrapped object, never on a raw eServiceReference):
#
#   class ServiceReference(eServiceReference):
#       def isRecordable(self):
#           ref = self.ref
#           return ref.flags & eServiceReference.isGroup or ref.type == eServiceReference.idDVB or ref.type == 4097 or ref.type == 8192
#
# Our own idServiceGstPlayer/idServiceExtEplayer3 types (5001/5002) are
# missing there, so RecordTimerEntry() silently discards any recording
# attempt for them before our own eServiceFactoryApp::record() is ever
# reached. Extending the method on the ServiceReference class (not
# eServiceReference) fixes this without touching any system file - every
# caller looks up the method fresh on every call, none of them cache it.
from ServiceReference import ServiceReference

_orig_service_reference_is_recordable = ServiceReference.isRecordable


def _serviceapp_is_recordable(self):
    return _orig_service_reference_is_recordable(self) or self.ref.type in (5001, 5002)


ServiceReference.isRecordable = _serviceapp_is_recordable


class ServiceAppPiconSync:
    @staticmethod
    def getPiconPaths():
        # ONLY local directories to prevent hanging on offline network shares (NFS/CIFS) at boot
        parent_dirs = ["/usr/share/enigma2", "/media/usb", "/media/hdd"]
        paths = []
        for parent in parent_dirs:
            if os.path.isdir(parent):
                try:
                    for name in os.listdir(parent):
                        # Matches picon, piconepg, picon_100x60, piconepg_50x30, etc.
                        if name.startswith(("picon", "piconepg")):
                            full_path = os.path.join(parent, name)
                            if os.path.isdir(full_path):
                                paths.append(full_path)
                except:
                    pass
        # Add config-specified directories just in case
        try:
            if hasattr(config.usage, "picon_dir") and config.usage.picon_dir.value:
                paths.append(config.usage.picon_dir.value)
            if hasattr(config.usage, "servicelist_picon_dir") and config.usage.servicelist_picon_dir.value:
                paths.append(config.usage.servicelist_picon_dir.value)
        except:
            pass
            
        # Clean paths (unique and valid)
        resolved = []
        for p in paths:
            if os.path.isdir(p) and p not in resolved:
                resolved.append(p)
        return resolved

    @staticmethod
    def runSync():
        if not config_serviceapp.picon_sync.value:
            ServiceAppPiconSync.cleanSync()
            return
            
        picon_dirs = ServiceAppPiconSync.getPiconPaths()
        if not picon_dirs:
            return
            
        # Parse TV bouquets
        bouquet_dir = "/etc/enigma2"
        refs_to_sync = set()
        
        try:
            for fn in os.listdir(bouquet_dir):
                if fn.startswith("userbouquet.") and fn.endswith(".tv"):
                    with open(os.path.join(bouquet_dir, fn), "r") as f:
                        for line in f:
                            if line.startswith("#SERVICE "):
                                ref_str = line.split(" ", 1)[1].strip()
                                # Check for 5001 or 5002 with valid DVB namespaces
                                if ref_str.startswith(("5001:", "5002:")):
                                    parts = ref_str.split(":")
                                    if len(parts) >= 10 and parts[3] != "0": # Valid SID
                                        refs_to_sync.add((parts[0], ":".join(parts[1:10])))
        except Exception as e:
            print("[ServiceAppPiconSync] Error parsing bouquets:", e)
            return

        # Create symlinks
        for prefix, ref_body in refs_to_sync:
            # Convert ref_body colons to underscores
            ref_name_body = ref_body.replace(":", "_")
            src_name = "1_" + ref_name_body + ".png"
            target_name = prefix + "_" + ref_name_body + ".png"
            
            for picon_dir in picon_dirs:
                src_path = os.path.join(picon_dir, src_name)
                target_path = os.path.join(picon_dir, target_name)
                
                # If target exists and is a symlink pointing to the source, skip
                if os.path.islink(target_path):
                    continue
                    
                # If target is a real file, do not overwrite it
                if os.path.exists(target_path):
                    continue
                    
                # If DVB source picon exists, create symlink
                if os.path.exists(src_path):
                    try:
                        os.symlink(src_name, target_path)
                        print("[ServiceAppPiconSync] Created symlink: %s -> %s" % (target_name, src_name))
                    except Exception as e:
                        print("[ServiceAppPiconSync] Failed to create symlink:", e)

    @staticmethod
    def cleanSync():
        # Remove all symlinks starting with 5001_ or 5002_ that point to 1_
        picon_dirs = ServiceAppPiconSync.getPiconPaths()
        for picon_dir in picon_dirs:
            try:
                for fn in os.listdir(picon_dir):
                    if fn.startswith(("5001_", "5002_")) and fn.endswith(".png"):
                        path = os.path.join(picon_dir, fn)
                        if os.path.islink(path):
                            target = os.readlink(path)
                            if target.startswith("1_"):
                                os.unlink(path)
                                print("[ServiceAppPiconSync] Removed symlink: %s" % fn)
            except Exception as e:
                print("[ServiceAppPiconSync] Clean error in %s:" % picon_dir, e)


SINKS_DEFAULT = ("dvbvideosink", "dvbaudiosink")
SINKS_EXPERIMENTAL = ("dvbvideosinkexp", "dvbaudiosinkexp")

sink_choices = []
if (os.path.isfile(eEnv.resolve("$libdir/gstreamer-1.0/libgstdvbvideosink.so")) and
        os.path.isfile(eEnv.resolve("$libdir/gstreamer-1.0/libgstdvbaudiosink.so"))):
    sink_choices.append("original")
if (os.path.isfile(eEnv.resolve("$libdir/gstreamer-1.0/libgstdvbvideosinkexp.so")) and
        os.path.isfile(eEnv.resolve("$libdir/gstreamer-1.0/libgstdvbaudiosinkexp.so"))):
    sink_choices.append("experimental")

player_choices = ["gstplayer", "exteplayer3"]
GSTPLAYER_VERSION = None
EXTEPLAYER3_VERSION = None

config.plugins.serviceapp                               = ConfigSubsection()
config_serviceapp                                       = config.plugins.serviceapp

config_serviceapp.servicemp3                            = ConfigSubsection()
config_serviceapp.servicemp3.replace                    = ConfigBoolean(default=False, descriptions={0: "original", 1: "serviceapp"})
config_serviceapp.servicemp3.replace.value              = serviceapp_client.isServiceMP3Replaced()
config_serviceapp.servicemp3.player                     = ConfigSelection(default="gstplayer", choices=player_choices)

config_serviceapp.debug_logging                         = ConfigBoolean(default=False)
config_serviceapp.picon_sync                            = ConfigBoolean(default=False)

config_serviceapp.options                               = ConfigSubDict()
config_serviceapp.options["servicemp3"]                 = ConfigSubsection()
config_serviceapp.options["servicegstplayer"]           = ConfigSubsection()
config_serviceapp.options["serviceexteplayer3"]         = ConfigSubsection()
for key in config_serviceapp.options.keys():
    config_serviceapp.options[key].hls_explorer         = ConfigBoolean(default=(key != "serviceexteplayer3"))
    config_serviceapp.options[key].autoselect_stream    = ConfigBoolean(default=True)
    config_serviceapp.options[key].connection_speed_kb  = ConfigInteger(9999999, limits=(0, 9999999))
    config_serviceapp.options[key].autoturnon_subtitles = ConfigBoolean(False)
    config_serviceapp.options[key].hls_audio_filter     = ConfigBoolean(default=True)

config_serviceapp.gstplayer                             = ConfigSubDict()
config_serviceapp.gstplayer["servicemp3"]               = ConfigSubsection()
config_serviceapp.gstplayer["servicegstplayer"]         = ConfigSubsection()
for key in config_serviceapp.gstplayer.keys():
    config_serviceapp.gstplayer[key].sink               = ConfigSelection(default="original", choices=sink_choices)
    config_serviceapp.gstplayer[key].buffer_size        = ConfigInteger(8192, (1024, 1024 * 64))
    config_serviceapp.gstplayer[key].buffer_duration    = ConfigInteger(0, (0, 100))
    config_serviceapp.gstplayer[key].subtitle_enabled   = ConfigBoolean(default=True)

config_serviceapp.exteplayer3                           = ConfigSubDict()
config_serviceapp.exteplayer3["servicemp3"]             = ConfigSubsection()
config_serviceapp.exteplayer3["serviceexteplayer3"]     = ConfigSubsection()
for key in config_serviceapp.exteplayer3.keys():
    config_serviceapp.exteplayer3[key].aac_swdecoding   = ConfigBoolean(default = False)
    config_serviceapp.exteplayer3[key].dts_swdecoding   = ConfigBoolean(default = True)
    config_serviceapp.exteplayer3[key].wma_swdecoding   = ConfigBoolean(default = False)
    config_serviceapp.exteplayer3[key].lpcm_injecion    = ConfigBoolean(default = False)
    config_serviceapp.exteplayer3[key].downmix          = ConfigBoolean(default = False)
    config_serviceapp.exteplayer3[key].hls_quality_mode = ConfigSelection(default=("highest" if key == "serviceexteplayer3" else "auto"),
        choices=[("auto", _("Auto (first in playlist)")), ("lowest", _("Lowest")), ("highest", _("Highest"))])
    config_serviceapp.exteplayer3[key].hls_audio_default_only = ConfigBoolean(default = True)
    config_serviceapp.exteplayer3[key].pcm_audio_export = ConfigBoolean(default = False)


def key_to_setting_id(key):
    setting_id = None
    if key == "servicemp3":
        setting_id = serviceapp_client.OPTIONS_SERVICEMP3
    elif key == "serviceexteplayer3":
        setting_id = serviceapp_client.OPTIONS_SERVICEEXTEPLAYER3
    elif key == "servicegstplayer":
        setting_id = serviceapp_client.OPTIONS_SERVICEGSTPLAYER
    return setting_id


def init_serviceapp_settings():
    debug_logging = config_serviceapp.debug_logging.value

    for key in config_serviceapp.options.keys():
        setting_id           = key_to_setting_id(key)
        serviceapp_cfg       = config_serviceapp.options[key]

        autoturnon_subtitles = serviceapp_cfg.autoturnon_subtitles.value
        hls_explorer         = serviceapp_cfg.hls_explorer.value
        autoselect_stream    = serviceapp_cfg.autoselect_stream.value
        connection_speed_kb  = serviceapp_cfg.connection_speed_kb.value
        hls_audio_filter     = serviceapp_cfg.hls_audio_filter.value

        serviceapp_client.setServiceAppSettings(setting_id, hls_explorer,
                autoselect_stream, connection_speed_kb, autoturnon_subtitles, hls_audio_filter,
                debug_logging)

    for key in config_serviceapp.gstplayer.keys():
        setting_id = key_to_setting_id(key)
        player_cfg = config_serviceapp.gstplayer[key]

        if player_cfg.sink.value == "original":
            video_sink, audio_sink = SINKS_DEFAULT
        elif player_cfg.sink.value == "experimental":
            video_sink, audio_sink = SINKS_EXPERIMENTAL
        else:
            continue
        subtitle_enabled = player_cfg.subtitle_enabled.value
        buffer_size      = player_cfg.buffer_size.value
        buffer_duration  = player_cfg.buffer_duration.value

        serviceapp_client.setGstreamerPlayerSettings(setting_id, video_sink, 
                audio_sink, subtitle_enabled, buffer_size, buffer_duration)

    for key in config_serviceapp.exteplayer3.keys():
        setting_id     = key_to_setting_id(key)
        player_cfg     = config_serviceapp.exteplayer3[key]

        aac_swdecoding = player_cfg.aac_swdecoding.value
        dts_swdecoding = player_cfg.dts_swdecoding.value
        wma_swdecoding = player_cfg.wma_swdecoding.value
        lpcm_injecion  = player_cfg.lpcm_injecion.value
        downmix        = player_cfg.downmix.value
        hls_quality_mode = {"auto": 0, "lowest": 1, "highest": 2}.get(player_cfg.hls_quality_mode.value, 0)
        hls_audio_default_only = player_cfg.hls_audio_default_only.value
        pcm_audio_export = player_cfg.pcm_audio_export.value

        serviceapp_client.setExtEplayer3Settings(setting_id, aac_swdecoding,
                dts_swdecoding, wma_swdecoding, lpcm_injecion, downmix,
                hls_quality_mode, hls_audio_default_only, debug_logging,
                pcm_audio_export)

    if config_serviceapp.servicemp3.player.value == "gstplayer":
        serviceapp_client.setServiceMP3GstPlayer()
    elif config_serviceapp.servicemp3.player.value == "exteplayer3":
        serviceapp_client.setServiceMP3ExtEplayer3()

init_serviceapp_settings()


class ServiceAppSettings(ConfigListScreen, Screen):
    def __init__(self, session):
        Screen.__init__(self, session)
        self.skinName = ["ServiceAppSettings", "Setup"]
        ConfigListScreen.__init__(self, [], session)
        self.setup_title = _("ServiceApp")
        self.onLayoutFinish.append(self.init_configlist)
        self.onClose.append(self.deinit_config)
        self["key_red"] = StaticText(_("Cancel"))
        self["key_green"] = StaticText(_("Ok"))
        self["description"] = Label("")
        self["setupActions"] = ActionMap(["SetupActions", "ColorActions"],
            {
                "cancel": self.keyCancel,
                "red": self.keyCancel,
                "ok": self.keyOk,
                "green": self.keyOk,
            }, -2)

    def init_configlist(self):
        config_serviceapp.servicemp3.player.addNotifier(
                lambda x: self.build_configlist(), initial_call=False)
        config_serviceapp.servicemp3.replace.addNotifier(
                lambda x: self.build_configlist(), initial_call=False)
        self.build_configlist()

    def deinit_config(self):
        del config_serviceapp.servicemp3.player.notifiers[:]
        del config_serviceapp.servicemp3.replace.notifiers[:]

    def gstplayer_options(self, gstplayer_options_cfg):
        config_list = []
        config_list.append(getConfigListEntry("  " + _("Sink"), 
            gstplayer_options_cfg.sink, _("Select sink which you want to use.")))
        config_list.append(getConfigListEntry("  " + _("Embedded subtitles"),
            gstplayer_options_cfg.subtitle_enabled, _("Turn on the embedded subtitles support.")))
        config_list.append(getConfigListEntry("  " + _("Buffer size"),
            gstplayer_options_cfg.buffer_size, _("Set buffer size in kilobytes.")))
        config_list.append(getConfigListEntry("  " + _("Buffer duration"),
            gstplayer_options_cfg.buffer_duration, _("Set buffer duration in seconds.")))
        return config_list

    def exteplayer3_options(self, exteplayer3_options_cfg):
        config_list = []
        config_list.append(getConfigListEntry("  " + _("AAC software decoding"),
            exteplayer3_options_cfg.aac_swdecoding, _("Turn on AAC software decoding.")))
        config_list.append(getConfigListEntry("  " + _("DTS software decoding"),
            exteplayer3_options_cfg.dts_swdecoding, _("Turn on DTS software decoding.")))
        config_list.append(getConfigListEntry("  " + _("WMA software decoding"),
            exteplayer3_options_cfg.wma_swdecoding, _("Turn on WMA1, WMA2, WMA/PRO software decoding.")))
        config_list.append(getConfigListEntry("  " + _("Stereo downmix"),
            exteplayer3_options_cfg.downmix, _("Turn on downmix to stereo, when software decoding is in use")))
        config_list.append(getConfigListEntry("  " + _("LPCM injection"),
            exteplayer3_options_cfg.lpcm_injecion, _("Software decoder use LPCM for injection (otherwise wav PCM will be used)")))
        config_list.append(getConfigListEntry("  " + _("HLS/DASH start quality"),
            exteplayer3_options_cfg.hls_quality_mode, _("Which HLS/DASH variant exteplayer3 should start with, when it parses the manifest/playlist itself.")))
        config_list.append(getConfigListEntry("  " + _("HLS default audio only"),
            exteplayer3_options_cfg.hls_audio_default_only, _("Only keep the DEFAULT=YES audio rendition per HLS audio group, when exteplayer3 parses the master playlist itself.")))
        config_list.append(getConfigListEntry("  " + _("PCM audio export for third-party plugins"),
            exteplayer3_options_cfg.pcm_audio_export, _("Writes decoded PCM audio to /tmp/exteplayer3_pcm_audio.fifo. Forces software decoding for common audio formats.")))
        return config_list

    def serviceapp_options(self, serviceapp_options_cfg):
        config_list = []
        config_list.append(getConfigListEntry("  " + _("Auto turn on subtitles"),
            serviceapp_options_cfg.autoturnon_subtitles, _("Automatically turn on subtitles if available.")))
        config_list.append(getConfigListEntry("  " + _("HLS Explorer"),
            serviceapp_options_cfg.hls_explorer, _("Turn on explorer to retrieve different quality streams from HLS variant playlist and select them via subservices.")))
        config_list.append(getConfigListEntry("  " + _("HLS audio track filter"),
            serviceapp_options_cfg.hls_audio_filter, _("Only keep the default audio track to improve performance on multi-audio HLS streams.")))
        config_list.append(getConfigListEntry("  " + _("Auto select stream"),
            serviceapp_options_cfg.autoselect_stream, _("Turn on auto-selection of streams according to set Connection speed.")))
        config_list.append(getConfigListEntry("  " + _("Connection speed"),
            serviceapp_options_cfg.connection_speed_kb, _("Set connection speed in kb/s, according to which you want to have streams auto-selected")))
        return config_list

    def player_options(self, player_type, service_type):
        config_list = []
        player_cfg = getattr(config_serviceapp, player_type)[service_type]
        serviceapp_cfg = config_serviceapp.options[service_type]
        if player_type == "exteplayer3":
            config_list.append(getConfigListEntry("  " + _("ExtEplayer3"),
                ConfigSelection([EXTEPLAYER3_VERSION or "not installed"], EXTEPLAYER3_VERSION or "not installed")))
            if EXTEPLAYER3_VERSION:
                config_list += self.exteplayer3_options(player_cfg)
                config_list += self.serviceapp_options(serviceapp_cfg)
        if player_type == "gstplayer":
            config_list.append(getConfigListEntry("  " + _("GstPlayer"), 
                ConfigSelection([GSTPLAYER_VERSION or "not installed"], GSTPLAYER_VERSION or "not installed")))
            if GSTPLAYER_VERSION:
                config_list += self.gstplayer_options(player_cfg)
                config_list += self.serviceapp_options(serviceapp_cfg)
        return config_list


    def build_configlist(self):
        config_list = [getConfigListEntry(_("Enigma2 playback system"),
            config_serviceapp.servicemp3.replace, _("Select the player which will be used for Enigma2 playback."))]
        config_list.append(getConfigListEntry(_("Debug logging"),
            config_serviceapp.debug_logging, _("Enable verbose diagnostic logging to /tmp/serviceapp.log (a RAM disk on this box). Leave off for normal use, only enable it temporarily when troubleshooting a playback issue.")))
        config_list.append(getConfigListEntry(_("Automated Picon Sync for 5001/5002"),
            config_serviceapp.picon_sync, _("Automatically create symlinks for 5001/5002 channels to standard DVB (1) picons.")))
        if config_serviceapp.servicemp3.replace.value:
            config_list.append(getConfigListEntry(_("Player"), 
                config_serviceapp.servicemp3.player, _("Select the player which will be used in serviceapp for Enigma2 playback.")))
            configlist_servicemp3 = [getConfigListEntry("", ConfigNothing())]
            configlist_servicemp3.append(getConfigListEntry(_("ServiceMp3 (%s)" % str(serviceapp_client.ID_SERVICEMP3)), ConfigNothing()))
            if config_serviceapp.servicemp3.player.value == "gstplayer":
                config_list += configlist_servicemp3 + self.player_options("gstplayer","servicemp3")
            elif config_serviceapp.servicemp3.player.value == "exteplayer3":
                config_list += configlist_servicemp3 + self.player_options("exteplayer3","servicemp3")
            else:
                config_list += configlist_servicemp3
        config_list.append(getConfigListEntry("", ConfigNothing()))
        config_list.append(getConfigListEntry(_("ServiceGstPlayer (%s)" % str(serviceapp_client.ID_SERVICEGSTPLAYER)), ConfigNothing()))
        config_list += self.player_options("gstplayer", "servicegstplayer")
        config_list.append(getConfigListEntry("", ConfigNothing()))
        config_list.append(getConfigListEntry(_("ServiceExtEplayer3 (%s)" % str(serviceapp_client.ID_SERVICEEXTEPLAYER3)), ConfigNothing()))
        config_list += self.player_options("exteplayer3", "serviceexteplayer3")
        self["config"].list = config_list
        self["config"].l.setList(config_list)

    def keyOk(self):
        if config_serviceapp.servicemp3.replace.isChanged():
            self.session.openWithCallback(self.save_settings_and_close, 
                    MessageBox, _("Enigma2 playback system was changed and Enigma2 should be restarted\n\nDo you want to restart it now?"),
                    type=MessageBox.TYPE_YESNO)
        else:
            self.save_settings_and_close()

    def save_settings_and_close(self, callback=False):
        init_serviceapp_settings()
        if config_serviceapp.servicemp3.replace.value:
            serviceapp_client.setServiceMP3Replace(True)
        else:
            serviceapp_client.setServiceMP3Replace(False)
        self.saveAll()
        try:
            ServiceAppPiconSync.runSync()
        except Exception as e:
            print("[ServiceApp] Picon sync on save failed:", e)
        self.close(callback)


class ServiceAppPlayer(MoviePlayer):
    def __init__(self, session, service):
        MoviePlayer.__init__(self, session, service)
        self.skinName = ["ServiceAppPlayer", "MoviePlayer"]
        self.servicelist = InfoBar.instance and InfoBar.instance.servicelist

    def handleLeave(self, how):
        if how == "ask":
            self.session.openWithCallback(self.leavePlayerConfirmed,
                    MessageBox, _("Stop playing this movie?"))
        else:
            self.close()

    def leavePlayerConfirmed(self, answer):
        if answer:
            self.close()


class ServiceAppDetectPlayers(Screen):
    skin = """
        <screen position="center,center" size="500,340" title="ServiceApp - player check">
            <widget name="text" position="10,10" size="490,325" font="Regular;28" halign="center" valign="center" />
        </screen>
                """
    def __init__(self, session):
        Screen.__init__(self, session)
        self["text"] = Label()
        self.players_iter = iter(
                [("gstplayer_gst-1.0", 
                    _("Detecting gstreamer player ..."),
                    self.detect_gstplayer),
                 ("exteplayer3", 
                     _("Detecting exteplayer3 player ..."),
                     self.detect_exteplayer3)
                 ])
        self.onLayoutFinish.append(self.detect_next_player)

    def detect_next_player(self):
        player = next(self.players_iter, None)
        if player is not None:
            self["text"].setText(player[1])
            self.console = Console()
            self.console.ePopen(player[0], boundFunction(self.detect_player_cb, player[2]))
        else:
            self.close()

    def detect_player_cb(self, datafnc, data, retval, extra_args):
        datafnc(data, retval, extra_args)
        self.detect_next_player()

    def _get_first_json_data_from_string(self, data):
        jsondata = None
        for line in data.splitlines():
            try:
                jsondata = json.loads(line)
                break
            except ValueError as e:
                pass
        return jsondata

    def detect_gstplayer(self, data, retval, extra_args):
        global GSTPLAYER_VERSION
        GSTPLAYER_VERSION = None
        jsondata = self._get_first_json_data_from_string(data)
        if jsondata is None:
            print "[ServiceApp] cannot detect gstplayer version(1)!"
            return
        try:
            GSTPLAYER_VERSION = jsondata["GSTPLAYER_EXTENDED"]["version"]
        except KeyError:
            print "[ServiceApp] cannot detect gstplayer version(2)!"
        else:
            print "[ServiceApp] found gstplayer - %d version" % GSTPLAYER_VERSION

    def detect_exteplayer3(self, data, retval, extra_args):
        global EXTEPLAYER3_VERSION
        EXTEPLAYER3_VERSION = None
        jsondata = self._get_first_json_data_from_string(data)
        if jsondata is None:
            print "[ServiceApp] cannot detect exteplayer3 version(1)!"
            return
        try:
            EXTEPLAYER3_VERSION = jsondata["EPLAYER3_EXTENDED"]["version"]
        except KeyError:
            print "[ServiceApp] cannot detect exteplayer3 version(2)!"
        else:
            print "[ServiceApp] found exteplayer3 - %d version" % EXTEPLAYER3_VERSION


def main(session, **kwargs):

    def restart_enigma2(restart=False):
        if restart:
            from Screens.Standby import TryQuitMainloop
            session.open(TryQuitMainloop, 3)

    def open_serviceapp_settings(callback=None):
        session.openWithCallback(restart_enigma2, ServiceAppSettings)

    session.openWithCallback(open_serviceapp_settings, ServiceAppDetectPlayers)


def menu(menuid, **kwargs):
    if menuid == "system":
        return [(_("ServiceApp"), main, "serviceapp_setup", None)]
    return []


def play_exteplayer3(session, service, **kwargs):
    ref = eServiceReference(5002, 0, service.getPath())
    session.open(ServiceAppPlayer, service=ref)


def play_gstplayer(session, service, **kwargs):
    ref = eServiceReference(5001, 0, service.getPath())
    session.open(ServiceAppPlayer, service=ref)


def autostart(reason, **kwargs):
    if reason == 0:  # Enigma2 Startup
        try:
            ServiceAppPiconSync.runSync()
        except Exception as e:
            print("[ServiceApp] Boot picon sync failed:", e)


def Plugins(**kwargs):
    return [
            PluginDescriptor(where=PluginDescriptor.WHERE_AUTOSTART, fnc=autostart),
            PluginDescriptor(name=_("ServiceApp"), description=_("setup player framework"),
                where=PluginDescriptor.WHERE_MENU, needsRestart=False, fnc=menu),
            PluginDescriptor(name=_("ServiceApp"), description=_("Play with ServiceExtEplayer3"),
                where=PluginDescriptor.WHERE_MOVIELIST, needsRestart=False, fnc=play_exteplayer3),
            PluginDescriptor(name=_("ServiceApp"), description=_("Play with ServiceGstPlayer"),
                where=PluginDescriptor.WHERE_MOVIELIST, needsRestart=False, fnc=play_gstplayer)
            ]
