/* 
 * Copyright (C) 2011 Chris McClelland
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *  
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <makestuff.h>
#include <libbuffer.h>
#include "xsvf2csvf.h"
#include "xsvf.h"

#define bitsToBytes(x) ((x>>3) + (x&7 ? 1 : 0))
#define CHECK_BUF_STATUS(x) if ( status != BUF_SUCCESS ) { returnCode = x; goto cleanup; }
#define CHECK_RETURN() if ( returnCode ) { goto cleanup; }
#define FAIL(x) returnCode = x; goto cleanup

#define ENABLE_SWAP
#define BUF_SIZE 128

// Global buffer and offset used to implement the iterator
//
typedef struct {
	struct Buffer xsvfBuf;
	uint32 offset;
} XC;

// The buffer iterator. TODO: refactor to return error code on end of buffer.
//
static uint8 getNextByte(XC *xc) {
	return xc->xsvfBuf.data[xc->offset++];
}

// Read "numBytes" bytes from the stream into a temporary buffer, then write them out in the reverse
// order to the supplied buffer "outBuf". If ENABLE_SWAP is undefined, no swapping is done, so the
// output should be identical to the input.
//
static X2CStatus swapBytes(XC *xc, uint32 numBytes, struct Buffer *outBuf, const char **error) {
	X2CStatus returnCode = X2C_SUCCESS;
	uint8 swapBuffer[2*BUF_SIZE];  // XSDRTDO accepts 2x XSDRSIZE bytes; all must be swapped
	uint8 *ptr = swapBuffer + numBytes - 1;
	uint32 n = numBytes;
	BufferStatus status;
	while ( n-- ) {
		*ptr-- = getNextByte(xc);
	}
	#ifdef ENABLE_SWAP
		ptr = swapBuffer;
		while ( numBytes-- ) {
			status = bufAppendByte(outBuf, *ptr++, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
		}
	#else
		ptr = swapBuffer + numBytes - 1;
		while ( numBytes-- ) {
			status = bufAppendByte(outBuf, *ptr--, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
		}
	#endif
cleanup:
	return returnCode;
}

// Parse the XSVF, reversing the byte-ordering of all the bytestreams.
//
static X2CStatus xsvfSwapBytes(XC *xc, struct Buffer *outBuf, uint16 *maxBufSize, const char **error) {
	X2CStatus returnCode = X2C_SUCCESS;
	uint16 xsdrSize = 0;
	uint16 numBytes;
	BufferStatus status;
	uint8 thisByte;

	*maxBufSize = 0;
	thisByte = getNextByte(xc);
	while ( thisByte != XCOMPLETE ) {
		switch ( thisByte ) {
		case XTDOMASK:
			// Swap the XTDOMASK bytes.
			numBytes = bitsToBytes(xsdrSize);
			if ( numBytes > BUF_SIZE ) {
				FAIL(X2C_UNSUPPORTED_SIZE_ERR);
			}
			if ( numBytes > *maxBufSize ) {
				*maxBufSize = numBytes;
			}
			status = bufAppendByte(outBuf, XTDOMASK, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			returnCode = swapBytes(xc, numBytes, outBuf, error);
			CHECK_RETURN();
			break;

		case XSDRTDO:
			// Swap the tdiValue and tdoExpected bytes.
			numBytes = bitsToBytes(xsdrSize);
			if ( numBytes > BUF_SIZE ) {
				FAIL(X2C_UNSUPPORTED_SIZE_ERR);
			}
			if ( numBytes > *maxBufSize ) {
				*maxBufSize = numBytes;
			}
			status = bufAppendByte(outBuf, XSDRTDO, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			returnCode = swapBytes(xc, 2*numBytes, outBuf, error);
			CHECK_RETURN();
			break;

		case XREPEAT:
			// Drop XREPEAT.
			getNextByte(xc);
			break;
			
		case XRUNTEST:
			// Copy the XRUNTEST bytes as-is.
			status = bufAppendByte(outBuf, XRUNTEST, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			status = bufAppendByte(outBuf, getNextByte(xc), error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			status = bufAppendByte(outBuf, getNextByte(xc), error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			status = bufAppendByte(outBuf, getNextByte(xc), error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			status = bufAppendByte(outBuf, getNextByte(xc), error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			break;

		case XSIR:
			// Swap the XSIR bytes.
			status = bufAppendByte(outBuf, XSIR, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			thisByte = getNextByte(xc);
			status = bufAppendByte(outBuf, thisByte, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			returnCode = swapBytes(xc, bitsToBytes(thisByte), outBuf, error);
			CHECK_RETURN();
			break;

		case XSDRSIZE:
			// The XSVF spec has the XSDRSIZE as a uint32. But since the bitstreams in XSVF files
			// are big-endian, they have to be buffered first, thus requiring RAM. I'm going to
			// stick my neck out and guess that Xilinx tools will never set XSDRSIZE to anything
			// bigger than 0xFFFF, so we can trim the uint32 down to uint16, and apply a further
			// size check in XSDRTDO and XTDOMASK to put a further limit on it.
			status = bufAppendByte(outBuf, XSDRSIZE, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			if ( getNextByte(xc) ) {
				FAIL(X2C_UNSUPPORTED_SIZE_ERR);  // Fail if either MSW bytes are nonzero
			}
			if ( getNextByte(xc) ) {
				FAIL(X2C_UNSUPPORTED_SIZE_ERR);
			}
			thisByte = getNextByte(xc);  // Get MSB
			xsdrSize = thisByte;
			status = bufAppendByte(outBuf, thisByte, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			thisByte = getNextByte(xc);  // Get LSB
			xsdrSize <<= 8;
			xsdrSize |= thisByte;
			status = bufAppendByte(outBuf, thisByte, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			break;

		case XSDRB:
			// Swap the tdiValue bytes.
			status = bufAppendByte(outBuf, XSDRB, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			returnCode = swapBytes(xc, bitsToBytes(xsdrSize), outBuf, error);
			CHECK_RETURN();
			break;

		case XSDRC:
			// Swap the tdiValue bytes.
			status = bufAppendByte(outBuf, XSDRC, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			returnCode = swapBytes(xc, bitsToBytes(xsdrSize), outBuf, error);
			CHECK_RETURN();
			break;

		case XSDRE:
			// Swap the tdiValue bytes.
			status = bufAppendByte(outBuf, XSDRE, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			returnCode = swapBytes(xc, bitsToBytes(xsdrSize), outBuf, error);
			CHECK_RETURN();
			break;

		case XSTATE:
			// Only switching to states TAPSTATE_TEST_LOGIC_RESET and TAPSTATE_RUN_TEST_IDLE are
			// supported so fail quickly if there's an attempt to switch to a different state.
			status = bufAppendByte(outBuf, XSTATE, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			thisByte = getNextByte(xc);
			if ( thisByte != TAPSTATE_TEST_LOGIC_RESET && thisByte != TAPSTATE_RUN_TEST_IDLE ) {
				FAIL(X2C_UNSUPPORTED_DATA_ERR);
			}
			status = bufAppendByte(outBuf, thisByte, error);
			CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			break;

		case XENDIR:
			// Only the default XENDIR state (TAPSTATE_RUN_TEST_IDLE) is supported. Fail fast if
			// there's an attempt to switch the XENDIR state to PAUSE_IR.
			thisByte = getNextByte(xc);
			if ( thisByte ) {
				FAIL(X2C_UNSUPPORTED_DATA_ERR);
			}
			break;

		case XENDDR:
			// Only the default XENDDR state (TAPSTATE_RUN_TEST_IDLE) is supported. Fail fast if
			// there's an attempt to switch the XENDDR state to PAUSE_DR.
			thisByte = getNextByte(xc);
			if ( thisByte ) {
				FAIL(X2C_UNSUPPORTED_DATA_ERR);
			}
			break;

		default:
			// All other commands are unsupported, so fail if they're encountered.
			FAIL(X2C_UNSUPPORTED_CMD_ERR);
		}
		thisByte = getNextByte(xc);
	}

	// Add the XCOMPLETE command
	status = bufAppendByte(outBuf, XCOMPLETE, error);
	CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
	
cleanup:
	return returnCode;
}

static X2CStatus compress(const struct Buffer *inBuf, struct Buffer *outBuf, const char **error) {
	X2CStatus returnCode = X2C_SUCCESS;
	const uint8 *runStart, *runEnd, *bufEnd, *chunkStart, *chunkEnd;
	uint32 runLen, chunkLen;
	BufferStatus status;
	bufEnd = inBuf->data + inBuf->length;
	runStart = chunkStart = inBuf->data;
	status = bufAppendByte(outBuf, 0x00, error);  // Hdr byte: defaults
	CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
	while ( runStart < bufEnd ) {
		// Find next zero
		while ( runStart < bufEnd && *runStart ) {
			runStart++;
		}
		
		// Remember the position of the zero
		runEnd = runStart;

		// Find the end of this run of zeros
		while ( runEnd < bufEnd && !*runEnd ) {
			runEnd++;
		}
		
		// Get the length of this run
		runLen = runEnd - runStart;
		
		// If this run is more than four zeros, break the chunk
		if ( runLen > 8 || runEnd == bufEnd ) {
			chunkEnd = runStart;
			chunkLen = chunkEnd - chunkStart;

			// There is now a chunk starting at chunkStart and ending at chunkEnd (length chunkLen),
			// Followed by a run of zeros starting at runStart and ending at runEnd (length runLen).
			//printf("Chunk: %d bytes followed by %d zeros\n", chunkLen, runLen);
			if ( chunkLen < 256 ) {
				// Short chunk: uint8
				status = bufAppendByte(outBuf, (uint8)chunkLen, error);
				CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			} else {
				// Long chunk: uint16 (big-endian)
				status = bufAppendByte(outBuf, 0x00, error);
				CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)((chunkLen>>8)&0x000000FF), error);
				CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)(chunkLen&0x000000FF), error);
				CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			}
			while ( chunkStart < chunkEnd ) {
				status = bufAppendByte(outBuf, *chunkStart++, error);
				CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			}
			if ( runLen < 256 ) {
				// Short run: uint8
				status = bufAppendByte(outBuf, (uint8)runLen, error);
				CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			} else {
				// Long run: uint16 (big-endian)
				status = bufAppendByte(outBuf, 0x00, error);
				CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)((runLen>>8)&0x000000FF), error);
				CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)(runLen&0x000000FF), error);
				CHECK_BUF_STATUS(X2C_BUF_APPEND_ERR);
			}

			chunkStart = runEnd;
		}
		
		// Start the next round from the end of this run
		runStart = runEnd;
	}

cleanup:
	return returnCode;
}

X2CStatus loadXsvfAndConvertToCsvf(
	const char *xsvfFile, struct Buffer *csvfBuf, uint16 *maxBufSize, const char **error)
{
	X2CStatus returnCode = X2C_SUCCESS;
	struct Buffer swapBuf;
	BufferStatus status;
	XC xc;
	xc.offset = 0;
	status = bufInitialise(&xc.xsvfBuf, 0x20000, 0, error);
	CHECK_BUF_STATUS(X2C_BUF_INIT_ERR);
	status = bufInitialise(&swapBuf, 0x20000, 0, error);
	CHECK_BUF_STATUS(X2C_BUF_INIT_ERR);
	status = bufAppendFromBinaryFile(&xc.xsvfBuf, xsvfFile, error);
	CHECK_BUF_STATUS(X2C_BUF_LOAD_ERR);
	returnCode = xsvfSwapBytes(&xc, &swapBuf, maxBufSize, error);
	CHECK_RETURN();
	returnCode = compress(&swapBuf, csvfBuf, error);
	CHECK_RETURN();
cleanup:
	bufDestroy(&swapBuf);
	bufDestroy(&xc.xsvfBuf);
	return returnCode;
}
