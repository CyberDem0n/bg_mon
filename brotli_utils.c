#include "brotli_utils.h"
#include "safety_funcs.h"

#define CHUNK 2048

static void free_fn(void *opaque, void *ptr)
{
	free(ptr);
}

void brotli_init(struct compression_state *state, int quality)
{
	if (!(state->state = BrotliEncoderCreateInstance(realloc_fn, free_fn, NULL)))
		exit_with_error("Failed to create BrotliEncoderInstance\n");
	BrotliEncoderSetParameter(state->state, BROTLI_PARAM_QUALITY, quality);
	state->pos = 0;
	state->is_finished = false;
}

void brotli_compress_data(struct compression_state *state, const unsigned char *next_in, size_t avail_in, BrotliEncoderOperation op)
{
	size_t avail_out;
	unsigned char *next_out;
	do {
		if (state->len <= state->pos) {
			state->len += CHUNK;
			state->to = realloc_fn(state->to, state->len);
		}

		avail_out = state->len - state->pos;
		next_out = state->to + state->pos;

		if (!BrotliEncoderCompressStream(state->state, op, &avail_in, &next_in, &avail_out, &next_out, NULL))
			exit_with_error("BrotliEncoderCompressStream() failed");

		state->pos += state->len - state->pos - avail_out;
	} while (avail_in > 0 || (op != BROTLI_OPERATION_FINISH && BrotliEncoderHasMoreOutput(state->state))
				|| (op == BROTLI_OPERATION_FINISH && !BrotliEncoderIsFinished(state->state)));

	state->is_flushed = op == BROTLI_OPERATION_FLUSH;
	if (op == BROTLI_OPERATION_FINISH) {
		BrotliEncoderDestroyInstance(state->state);
		state->is_finished = true;
	}
}

void brotli_compress_evbuffer(struct compression_state *state, struct evbuffer *from, BrotliEncoderOperation op)
{
	size_t avail_in;
	const unsigned char *next_in;
	struct evbuffer_ptr ptr;
	struct evbuffer_iovec v_in[1] = {0, };
	bool is_eof = !from;

	if (!is_eof && evbuffer_ptr_set(from, &ptr, 0, EVBUFFER_PTR_SET) < 0)
		exit_with_error("evbuffer_ptr_set() failed");

	do {
		if (!is_eof && evbuffer_ptr_set(from, &ptr, v_in[0].iov_len, EVBUFFER_PTR_ADD) < 0)
			is_eof = true;

		if (!is_eof && evbuffer_peek(from, -1, &ptr, v_in, 1)) {
			avail_in = v_in[0].iov_len;
			next_in = (unsigned char *)v_in[0].iov_base;
		} else {
			avail_in = 0;
			next_in = NULL;
		}

		if (avail_in == 0 && !is_eof)
			is_eof = true;

		brotli_compress_data(state, next_in, avail_in, is_eof ? op : BROTLI_OPERATION_PROCESS);
	} while (!is_eof);
}

void brotli_dealloc(struct compression_state *state)
{
	if (!state->is_finished)
		BrotliEncoderDestroyInstance(state->state);
}

void brotli_cleanup(const void *data, size_t datalen, void *extra)
{
	struct compression_state *state = (struct compression_state *) extra;
	if (state->to) {
		free(state->to);
		state->to = NULL;
		state->len = 0;
	}
	brotli_dealloc(state);
	free(state);
}
