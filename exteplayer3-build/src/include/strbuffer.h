#ifndef STRBUFFER_H_
#define STRBUFFER_H_

typedef struct buffer
{
	char         *buffer;
	size_t        size;
	char         *pos;
	char         *end;
	size_t        init_size;
} strbuffer_t;

#define STRBUFFER_USED( buff ) \
	((size_t) ((buff)->pos - (buff)->buffer))

strbuffer_t *strbuffer_init( size_t init_size );
int strbuffer_free( strbuffer_t *buffer );

int strbuffer_init2( strbuffer_t *buffer, size_t init_size );
int strbuffer_free2( strbuffer_t *buffer );

int strbuffer_clear( strbuffer_t *buffer );

int strbuffer_add( strbuffer_t *buffer, const char *str );
int strbuffer_add_len( strbuffer_t *buffer, const char *str, size_t str_len );


#endif /* STRBUFFER_H_ */
