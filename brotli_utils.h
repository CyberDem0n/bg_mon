#ifndef _BROTLI_UTILS_H_
#define _BROTLI_UTILS_H_
#include <event2/buffer.h>
#include <brotli/encode.h>

#include "postgres.h"

struct compression_state {
	BrotliEncoderState *state;
	unsigned char *to;
	size_t len;
	size_t pos;
	bool is_flushed;
	bool is_finished;
};

void brotli_init(struct compression_state *state, int quality);

void brotli_compress_evbuffer(struct compression_state *state, struct evbuffer *from, BrotliEncoderOperation op);

void brotli_compress_data(struct compression_state *state, const unsigned char *next_in, size_t avail_in, BrotliEncoderOperation op);

void brotli_dealloc(struct compression_state *state);

void brotli_cleanup(const void *data, size_t datalen, void *extra);

#endif /* _BROTLI_UTILS_H_ */
