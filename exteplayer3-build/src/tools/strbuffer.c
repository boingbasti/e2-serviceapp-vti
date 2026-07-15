#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "strbuffer.h"

strbuffer_t *strbuffer_init( size_t init_size )
{
	strbuffer_t *buffer;

	if( !init_size )
	{
		return NULL;
	}


	if( (buffer = malloc( sizeof( strbuffer_t ) )) == NULL )
	{
		return NULL;
	}

	strbuffer_init2( buffer, init_size );

	return buffer;
}


int strbuffer_init2( strbuffer_t *buffer, size_t init_size )
{
	if( !buffer || !init_size )
	{
		return( -1 );
	}

	memset( buffer, 0, sizeof( strbuffer_t ) );
	(buffer->init_size) = init_size;

	return 0;
}


int strbuffer_free( strbuffer_t *buffer )
{
	if( !buffer )
	{
		return( -1 );
	}

	strbuffer_free2( buffer );
	free( buffer );

	return( 0 );
}


int strbuffer_free2( strbuffer_t *buffer )
{
	if( !buffer )
	{
		return( -1 );
	}

	if( buffer->buffer )
	{
		free( buffer->buffer );
		buffer->buffer = NULL;
	}

	return( 0 );
}


int strbuffer_clear( strbuffer_t *buffer )
{
	if( !buffer )
	{
		return -1;
	}

	buffer->pos = buffer->buffer;

	return 0;
}

int strbuffer_add( strbuffer_t *buffer, const char *str )
{
	if( !buffer || !str )
	{
		return( -1 );
	}

	return strbuffer_add_len( buffer, str, strlen( str ) );
}


int strbuffer_add_len( strbuffer_t *buffer, const char *str, size_t str_len )
{
	if( !buffer || !str )
	{
		return( -1 );
	}

	if( !buffer->buffer || (buffer->end - buffer->pos) <= str_len )
	{
		size_t used = (buffer->pos) - (buffer->buffer);

		do
		{
			buffer->size += buffer->init_size;
		}
		while( (buffer->size - used) < str_len );


		if( (buffer->buffer = realloc( buffer->buffer, buffer->size )) == NULL )
		{
			return( -1 );
		}
		buffer->pos = buffer->buffer + used;
		buffer->end = buffer->buffer + buffer->size;
	}

	memcpy( buffer->pos, str, str_len );
	buffer->pos += str_len;
	*(buffer->pos) = '\0';

	return( 0 );
}

