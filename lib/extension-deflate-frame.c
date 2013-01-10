#include "private-libwebsockets.h"
#include "extension-deflate-frame.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define LWS_ZLIB_WINDOW_BITS 15
#define LWS_ZLIB_MEMLEVEL 8

#define MIN_SIZE_TO_DEFLATE 4


int lws_extension_callback_deflate_frame(
		struct libwebsocket_context *context,
		struct libwebsocket_extension *ext,
		struct libwebsocket *wsi,
		enum libwebsocket_extension_callback_reasons reason,
		void *user, void *in, size_t len)
{
	struct lws_ext_deflate_frame_conn *conn =
					 (struct lws_ext_deflate_frame_conn *)user;
	struct lws_tokens *eff_buf = (struct lws_tokens *)in;
	size_t current_payload, remaining_payload, total_payload;
	int n;

	switch (reason)
	{

	/*
	 * for deflate-frame, both client and server sides act the same
	 */

	case LWS_EXT_CALLBACK_CLIENT_CONSTRUCT:
	case LWS_EXT_CALLBACK_CONSTRUCT:
		conn->zs_in.zalloc = conn->zs_out.zalloc = Z_NULL;
		conn->zs_in.zfree = conn->zs_out.zfree = Z_NULL;
		conn->zs_in.opaque = conn->zs_out.opaque = Z_NULL;
		n = inflateInit2(&conn->zs_in, -LWS_ZLIB_WINDOW_BITS);
		if (n != Z_OK)
		{
			lws_log(LWS_LOG_WARNING, "deflateInit returned %d", n);
			return 1;
		}
		n = deflateInit2(&conn->zs_out,
				 DEFLATE_FRAME_COMPRESSION_LEVEL, Z_DEFLATED,
				 -LWS_ZLIB_WINDOW_BITS, LWS_ZLIB_MEMLEVEL,
				 Z_DEFAULT_STRATEGY);
		if (n != Z_OK)
		{
			lws_log(LWS_LOG_WARNING, "deflateInit returned %d", n);
			return 1;
		}
		conn->buf_pre_used = 0;
		conn->buf_pre_length = 0;
		conn->buf_in_length = MAX_USER_RX_BUFFER;
		conn->buf_out_length = MAX_USER_RX_BUFFER;
		conn->compressed_out = 0;
		conn->buf_pre = NULL;
		conn->buf_in = (unsigned char *)malloc(LWS_SEND_BUFFER_PRE_PADDING +
						       conn->buf_in_length +
						       LWS_SEND_BUFFER_POST_PADDING);
		conn->buf_out = (unsigned char *)malloc(LWS_SEND_BUFFER_PRE_PADDING +
							conn->buf_out_length +
							LWS_SEND_BUFFER_POST_PADDING);
		lws_log(LWS_LOG_DEBUG, "zlibs constructed");
		break;

	case LWS_EXT_CALLBACK_DESTROY:
		if (conn->buf_pre)
		{
			free(conn->buf_pre);
		}
		free(conn->buf_in);
		free(conn->buf_out);
		conn->buf_pre_used = 0;
		conn->buf_pre_length = 0;
		conn->buf_in_length = 0;
		conn->buf_out_length = 0;
		conn->compressed_out = 0;
		(void)inflateEnd(&conn->zs_in);
		(void)deflateEnd(&conn->zs_out);
		lws_log(LWS_LOG_DEBUG, "zlibs destructed");
		break;

	case LWS_EXT_CALLBACK_PAYLOAD_RX:
		if ((wsi->rsv & 0x40) == 0)
		{
			return 0;
		}

		/*
		 * inflate the incoming payload
		 */
		current_payload = eff_buf->token_len;

		remaining_payload = wsi->rx_packet_length;
		if (remaining_payload)
		{
			total_payload = conn->buf_pre_used +
					current_payload +
					remaining_payload;

			if (conn->buf_pre_length < total_payload)
			{
				conn->buf_pre_length = total_payload;
				if (conn->buf_pre)
				{
					free(conn->buf_pre);
				}
				conn->buf_pre = (unsigned char *)malloc(total_payload + 4);
			}

			memcpy(conn->buf_pre + conn->buf_pre_used, eff_buf->token, current_payload);
			conn->buf_pre_used += current_payload;

			eff_buf->token = NULL;
			eff_buf->token_len = 0;

			return 0;
		}
		else if (conn->buf_pre_used)
		{
			total_payload = conn->buf_pre_used +
					current_payload;

			memcpy(conn->buf_pre + conn->buf_pre_used, eff_buf->token, current_payload);
			conn->buf_pre_used = 0;

			conn->zs_in.next_in = conn->buf_pre;
		}
		else
		{
			total_payload = current_payload;

			conn->zs_in.next_in = (unsigned char *)eff_buf->token;
		}

		conn->zs_in.next_in[total_payload + 0] = 0;
		conn->zs_in.next_in[total_payload + 1] = 0;
		conn->zs_in.next_in[total_payload + 2] = 0xff;
		conn->zs_in.next_in[total_payload + 3] = 0xff;

		conn->zs_in.avail_in = total_payload + 4;

		conn->zs_in.next_out = conn->buf_in + LWS_SEND_BUFFER_PRE_PADDING;
		conn->zs_in.avail_out = conn->buf_in_length;

		while (1)
		{
			n = inflate(&conn->zs_in, Z_SYNC_FLUSH);
			switch (n)
			{
			case Z_NEED_DICT:
			case Z_STREAM_ERROR:
			case Z_DATA_ERROR:
			case Z_MEM_ERROR:
				/*
				 * screwed.. close the connection... we will get a
				 * destroy callback to take care of closing nicely
				 */
				lws_log(LWS_LOG_ERROR, "zlib error inflate %d: %s", n, conn->zs_in.msg);
				return -1;
			}

			if (conn->zs_in.avail_out == 0)
			{
				size_t len_so_far = (conn->zs_in.next_out - (conn->buf_in + LWS_SEND_BUFFER_PRE_PADDING));
				unsigned char *new_buf;
				conn->buf_in_length *= 2;
				new_buf = (unsigned char *)malloc(LWS_SEND_BUFFER_PRE_PADDING +
								  conn->buf_in_length +
								  LWS_SEND_BUFFER_POST_PADDING);
				memcpy(new_buf + LWS_SEND_BUFFER_PRE_PADDING,
					conn->buf_in + LWS_SEND_BUFFER_PRE_PADDING,
					len_so_far);
				free(conn->buf_in);
				conn->buf_in = new_buf;
				conn->zs_in.next_out = (new_buf + LWS_SEND_BUFFER_PRE_PADDING + len_so_far);
				conn->zs_in.avail_out = (conn->buf_in_length - len_so_far);
			}
			else
			{
				break;
			}
		}

		/* rewrite the buffer pointers and length */
		eff_buf->token = (char *)(conn->buf_in + LWS_SEND_BUFFER_PRE_PADDING);
		eff_buf->token_len = (int)(conn->zs_in.next_out - (conn->buf_in + LWS_SEND_BUFFER_PRE_PADDING));

		return 0;

	case LWS_EXT_CALLBACK_PAYLOAD_TX:
		/*
		 * deflate the outgoing payload
		 */
		current_payload = eff_buf->token_len;

		if (current_payload < MIN_SIZE_TO_DEFLATE)
		{
			return 0;
		}

		conn->zs_out.next_in = (unsigned char *)eff_buf->token;
		conn->zs_out.avail_in = current_payload;

		conn->zs_out.next_out = conn->buf_out + LWS_SEND_BUFFER_PRE_PADDING;
		conn->zs_out.avail_out = conn->buf_out_length;

		while (1)
		{
			n = deflate(&conn->zs_out, Z_SYNC_FLUSH);
			if (n == Z_STREAM_ERROR)
			{
				/*
				 * screwed.. close the connection... we will get a
				 * destroy callback to take care of closing nicely
				 */
				lws_log(LWS_LOG_ERROR, "zlib error deflate");

				return -1;
			}

			if (conn->zs_out.avail_out == 0)
			{
				size_t len_so_far = (conn->zs_out.next_out - (conn->buf_out + LWS_SEND_BUFFER_PRE_PADDING));
				unsigned char *new_buf;
				conn->buf_out_length *= 2;
				new_buf = (unsigned char *)malloc(LWS_SEND_BUFFER_PRE_PADDING +
								  conn->buf_out_length +
								  LWS_SEND_BUFFER_POST_PADDING);
				memcpy(new_buf + LWS_SEND_BUFFER_PRE_PADDING,
					conn->buf_out + LWS_SEND_BUFFER_PRE_PADDING,
					len_so_far);
				free(conn->buf_out);
				conn->buf_out = new_buf;
				conn->zs_out.next_out = (new_buf + LWS_SEND_BUFFER_PRE_PADDING + len_so_far);
				conn->zs_out.avail_out = (conn->buf_out_length - len_so_far);
			}
			else
			{
				break;
			}
		}

		conn->compressed_out = 1;

		/* rewrite the buffer pointers and length */
		eff_buf->token = (char *)(conn->buf_out + LWS_SEND_BUFFER_PRE_PADDING);
		eff_buf->token_len = (int)(conn->zs_out.next_out - (conn->buf_out + LWS_SEND_BUFFER_PRE_PADDING)) - 4;

		return 0;

	case LWS_EXT_CALLBACK_PACKET_TX_PRESEND:
		if (conn->compressed_out)
		{
			conn->compressed_out = 0;
			*((unsigned char *)eff_buf->token) |= 0x40;
		}
		break;

	case LWS_EXT_CALLBACK_CHECK_OK_TO_PROPOSE_EXTENSION:
		/* Avoid x-webkit-deflate-frame extension on client */
		if (strcmp((char *)in, "x-webkit-deflate-frame") == 0)
		{
			return 1;
		}
		break;

	default:
		break;
	}

	return 0;
}
