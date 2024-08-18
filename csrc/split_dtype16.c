#define PY_SSIZE_T_CLEAN
#include "huf.h"
#include "huf_wrapper.h"
#include "split_dtype_functions.h"
#include <Python.h>
#include <stdint.h>
#include <time.h>

///////////////////////////////////
/// Split Helper Functions ///////
//////////////////////////////////

// Reordering function for float bits
static uint32_t reorder_float_bits(float number) {
  union {
    float f;
    uint32_t u;
  } value = {.f = number};

  uint32_t sign = (value.u >> 8) & 0x800080;
  uint32_t exponent = (value.u << 1) & 0xFF00FF00;
  uint32_t mantissa = (value.u) & 0x7F007F;
  return exponent | sign | mantissa;
}

// Helper function to reorder all floats in a bytearray
static void reorder_all_floats(u_int8_t *src, Py_ssize_t len) {
  uint32_t *uint_array = (uint32_t *)src;
  Py_ssize_t num_floats = len / sizeof(uint32_t);
  for (Py_ssize_t i = 0; i < num_floats; i++) {
    uint_array[i] = reorder_float_bits(*(float *)&uint_array[i]);
  }
}

// Helper function to split a bytearray into groups
static int split_bytearray(u_int8_t *src, Py_ssize_t len, u_int8_t **buffers,
                           int bits_mode, int bytes_mode, int is_review,
                           int threads) {
  if (bits_mode == 1) {  // reoreder exponent
    reorder_all_floats(src, len);
  }

  Py_ssize_t half_len = len / 2;
  switch (bytes_mode) {
  case 10:  // 2b01_010 - Byte Group to two different groups
    buffers[0] = PyMem_Malloc(half_len);
    buffers[1] = PyMem_Malloc(half_len);

    if (buffers[0] == NULL || buffers[1] == NULL) {
      PyMem_Free(buffers[0]);
      PyMem_Free(buffers[1]);
      return -1;
    }

    u_int8_t *dst0 = buffers[0];
    u_int8_t *dst1 = buffers[1];

    for (Py_ssize_t i = 0; i < len; i += 2) {
      *dst0++ = src[i];
      *dst1++ = src[i + 1];
    }
    break;

  case 8:  // 4b1000 - Truncate MSByte
           // We are refering to the MSBbyte as little endian, thus we omit buf2
  case 1:  // 4b1000 - Truncate LSByte
    // We are refering to the LSByte  as a little endian, thus we omit buf1
    buffers[0] = PyMem_Malloc(half_len);
    buffers[1] = NULL;

    if (buffers[0] == NULL) {
      PyMem_Free(buffers[0]);
      return -1;
    }

    dst0 = buffers[0];

    if (bytes_mode == 1) {
      for (Py_ssize_t i = 0; i < len; i += 2) {
        *dst0++ = src[i];
      }
    } else {
      for (Py_ssize_t i = 0; i < len; i += 2) {
        *dst0++ = src[i + 1];
      }
    }
    break;

  default:
    // we are not support this splitting bytes_mode
    return -1;
  }
  return 0;
}

///////////////////////////////////
/////////  Combine Functions //////
///////////////////////////////////

// Reordering function for float bits
static uint32_t revert_float_bits(float number) {
  union {
    float f;
    uint32_t u;
  } value = {.f = number};

  uint32_t sign = (value.u << 8) & 0x80008000;
  uint32_t exponent = (value.u >> 1) & 0x7F807F80;
  uint32_t mantissa = (value.u) & 0x7F007F;
  return sign | exponent | mantissa;
}

// Helper function to reorder all floats in a bytearray
static void revert_all_floats(u_int8_t *src, Py_ssize_t len) {
  uint32_t *uint_array = (uint32_t *)src;
  Py_ssize_t num_floats = len / sizeof(uint32_t);
  for (Py_ssize_t i = 0; i < num_floats; i++) {
    uint_array[i] = revert_float_bits(*(float *)&uint_array[i]);
  }
}

// Helper function to combine four buffers into a single bytearray
static u_int8_t *combine_buffers(u_int8_t *buf1, u_int8_t *buf2,
                                Py_ssize_t half_len, int bytes_mode,
                                int threads) {
  Py_ssize_t total_len = half_len * 2;
  u_int8_t *result = NULL;  // Declare result at the beginning of the function
  u_int8_t *dst;
  result = PyMem_Malloc(total_len);
  if (result == NULL) {
    PyErr_SetString(PyExc_MemoryError, "Failed to allocate memory");
    PyMem_Free(result);
    return NULL;
  }
  dst = result;

  switch (bytes_mode) {
  case 10:  // 2b01_010 - Byte Group to two different groups

    if (result == NULL) {

      return NULL;
    }

    for (Py_ssize_t i = 0; i < half_len; i++) {
      *dst++ = buf1[i];
      *dst++ = buf2[i];
    }
    break;

  case 8:  // 4b1000 - Truncate MSByte
           // We are refering to the MSByte as a little endian, thus we omit buf2
  case 1:  // 4b001 - Truncate LSByte
           // We are refering to the LSByte as a little endian, thus we omit buf1

    if (bytes_mode == 8) {
      for (Py_ssize_t i = 0; i < half_len; i++) {
        *dst++ = 0;
        *dst++ = buf1[i];
      }
    } else {
      for (Py_ssize_t i = 0; i < half_len; i++) {
        *dst++ = buf1[i];
        *dst++ = 0;
      }
    }
    break;

  default:
    // we are not supportin this splitting bytes_mode
    return NULL;
  }
  return result;
}

/////////////////////////////////////////////////////////////
//////////////// Python callable Functions /////////////////
/////////////////////////////////////////////////////////////

// Python callable function to split a bytearray into four buffers
// bits_mode:
//     0 - no ordering of the bits
//     1 - reorder of the exponent (eponent, sign_bit, mantissa)
// bytes_mode:
//     [we are refering to the bytes order as first 2bits refer to the MSByte
//     and the second two bits to the LSByte] 2b [MSB Byte],2b[LSB Byte] 0 -
//     truncate this byte 1 or 2 - a group of bytes 4b0110 [6] - bytegroup to
//     two groups 4b0001 [1] - truncate the MSByte 4b1000 [8] - truncate the
//     LSByte
// is_review:
//     Even if you have the Byte mode, you can change it if needed.
//     0 - No review, take the bit_mode and byte_mode
//     1 - the finction can change the Bytes_mode

PyObject *py_split_dtype16(PyObject *self, PyObject *args) {
  const uint32_t numBuf = 2;
  Py_buffer view;
  int bits_mode, bytes_mode, is_review, threads;
  u_int8_t isPrint = 0;
  clock_t startTime, endTime, startBGTime, endBGTime, startCompBufTime[numBuf],
      endCompBufTime[numBuf];
  double totalTime, bgTime, compBufTime[numBuf];

  startTime = clock();

  if (!PyArg_ParseTuple(args, "y*iiii", &view, &bits_mode, &bytes_mode,
                        &is_review, &threads)) {
    return NULL;
  }
 
  size_t size = view.len;
  u_int8_t *buffers[] = {NULL, NULL};
  uint32_t bgChunkSize = 256 * 1024;  
  uint32_t CompChunkSize = bgChunkSize / numBuf;  // TBD
  float compThreshold = 0.95;        // TBD
  size_t checkThreshold = 10;        // TBD

  size_t bufSize = size / numBuf;

  size_t numChunks = (size + bgChunkSize - 1) / bgChunkSize;
  size_t bufNumChunks[numBuf];
 
  u_int8_t isBufComp[numBuf];

  // Byte Group per chunk, Compress per bufChunk

  size_t curChunk = 0;
  size_t totalCompressedSize[] = {0, 0};

  u_int8_t* compressedData[numBuf][numChunks];
  uint32_t compChunksSize[numBuf][numChunks];
  u_int8_t compChunkType[numBuf][numChunks];
  uint32_t unCompChunksSize[numChunks];

  if (isPrint) {
      startBGTime = clock();
    }
 

  /////////// start multi Threading - Each chunk to different thread /////////////// 
  for (size_t offset = 0; offset < size; offset += bgChunkSize) {
    size_t curBgChunkSize =
      (view.len - offset > bgChunkSize) ? bgChunkSize : (size - offset);

//    printf ("offset %zu\n", offset);
    size_t curCompChunkSize = curBgChunkSize/ numBuf;
    unCompChunksSize[curChunk] = curCompChunkSize; 
    // Byte Grouping + Byte Ordering
   
    if (split_bytearray(view.buf+offset, curBgChunkSize, buffers, bits_mode, bytes_mode,
                    is_review, threads) != 0) {
      PyBuffer_Release(&view);
      PyErr_SetString(PyExc_MemoryError, "Failed to allocate memory");
      return NULL;
    }
    if (isPrint) {
      endBGTime = clock();
      bgTime = (double)(endBGTime - startBGTime) / CLOCKS_PER_SEC;
    }

    // Compression on each Buf
    
    for (uint32_t b = 0; b < numBuf; b++) {

      compressedData[b][curChunk] = PyMem_Malloc(bgChunkSize);
      if (!compressedData[b][curChunk]) {
        PyErr_SetString(PyExc_MemoryError, "Failed to allocate memory");
        exit(0);
          for (uint32_t j = 0; j < numBuf; j++) {
	   for (uint32_t c = 0; c < curChunk-1; c++) {
             PyMem_Free(compressedData[j][c]);
	   }
           for (uint32_t j = 0; j < b; j++) {
             PyMem_Free(compressedData[j][curChunk]);
	   }
	}
        return NULL;
      }

      if (buffers[b] != NULL) {
	compChunksSize[b][curChunk] =
          HUF_compress(compressedData[b][curChunk], bgChunkSize,
                     buffers[b], curCompChunkSize);

        if (compChunksSize[b][curChunk] != 0 || (compChunksSize[b][curChunk] > unCompChunksSize[curChunk] * compThreshold)) {
          totalCompressedSize[b] += compChunksSize[b][curChunk];
	  compChunkType[b][curChunk] = 1; // Compress with Huffman
        }
        else { // the buffer was not compressed 
          totalCompressedSize[b] += unCompChunksSize[curChunk];
	   compChunkType[b][curChunk] = 0; // not compressed
	}
	printf ("compChunkSize[%d][%d] %zu \n", b, curChunk, compChunksSize[b][curChunk]);
	printf ("compChunkType[%d][%d] %zu \n", b, curChunk, compChunkType[b][curChunk]);
	printf ("totalCompressedSize[%d] %zu \n", b, totalCompressedSize[b]);
      }
//      if (isPrint) {
//        endCompBufTime[i] = clock();
//        compBufTime[i] =
//		(double)(endCompBufTime[b] - startCompBufTime[b]) / CLOCKS_PER_SEC;
//         printf("totalCompressedSize[%d] %zu [%f] time %f  \n", b,
//			 totalCompressedSize[b], totalCompressedSize[b] * 1.0 / curCompChunkSize,
//			 compBufTime[b]);
//      }
    }  // end for loop -> compression
    curChunk++;
//    printf ("buffer size %zu\n", offset);
  } // end for loop - chunk 
  ////////////// The end of multi Threading ////////////////////////////// 

   
//  if (totalCompressedSize[i] == 0) {  // This buffer was not compressed
//          isBufComp[i] = 0;
//          bufNumChunks[i] = 0;
//          compChunksSize[i] = 0;
//        }



  endTime = clock();
  totalTime =
	  (double)(endTime - startTime) / CLOCKS_PER_SEC;
  printf("totalCompressedSize[0] %zu [%f] time %f  \n", totalCompressedSize[0], totalCompressedSize[0] * 1.0/ (view.len/numBuf), totalTime);
  printf("totalCompressedSize[1] %zu [%f] time %f  \n", totalCompressedSize[1], totalCompressedSize[1] * 1.0/ (view.len/numBuf), totalTime);
  printf ("number of chunks, %zu\n",numChunks);
  exit(0);
/*
  for (uint32_t i = 0; i < numBuf; i++) {
    isBufComp[i] = 1;
    bufNumChunks[i] = numChunks;
    if (isPrint) {
      startCompBufTime[i] = clock();
    }
    if (buffers[i] != NULL) {
      totalCompressedSize[i] = hufCompressData(
          buffers[i], bufSize, maxCompressedSize, compressedData[i],
          compChunksSize[i], chunkSize, compThreshold, checkThreshold);
      if (totalCompressedSize[i] == 0) {  // This buffer was not compressed
        isBufComp[i] = 0;
        bufNumChunks[i] = 0;
        compChunksSize[i] = NULL;
      }
    }
    if (isPrint) {
      endCompBufTime[i] = clock();
      compBufTime[i] =
          (double)(endCompBufTime[i] - startCompBufTime[i]) / CLOCKS_PER_SEC;
      printf("totalCompressedSize[%d] %zu [%f] time %f  \n", i,
             totalCompressedSize[i], totalCompressedSize[i] * 1.0 / bufSize,
             compBufTime[i]);
    }
  }

  PyObject *result;
  u_int8_t *resBuf[numBuf];
  size_t resBufSize[numBuf];

  if (buffers[1] != NULL) {
    for (uint32_t i = 0; i < numBuf; i++) {
      if (isBufComp[i]) {  // The buffer was compressed
        resBuf[i] = compressedData[i];
        resBufSize[i] = totalCompressedSize[i];
      } else {  // The buffer was not compressed
        resBuf[i] = buffers[i];
        resBufSize[i] = bufSize;
      }
    }
    u_int8_t item;
    memcpy(&item, resBuf[0], sizeof(u_int8_t));

    result = Py_BuildValue(
        "y#y#y#y#y#y#", resBuf[0], resBufSize[0], resBuf[1], resBufSize[1],
        &isBufComp[0], 1, &isBufComp[1], 1, compChunksSize[0],
        sizeof(size_t) * bufNumChunks[0], compChunksSize[1],
        sizeof(size_t) * bufNumChunks[1]);
  } else {
    PyErr_SetString(PyExc_MemoryError, "In split16 return two buffers, still "
                                       "not implemented to return 1 Buffer");
    result = Py_BuildValue("y#O", buffers[0], view.len / numBuf, Py_None);
  }

  for (uint32_t i = 0; i < numBuf; i++) {
    PyMem_Free(compressedData[i]);
    PyMem_Free(compChunksSize[i]);
    if (buffers[i] != NULL) {
      PyMem_Free(buffers[i]);
    }
  }
  PyBuffer_Release(&view);

  if (isPrint) {
    endTime = clock();
    double compressTime = (double)(endTime - startTime) / CLOCKS_PER_SEC;
    printf("original_size %zu \n", view.len);
    printf("BG compression time: %f seconds\n", bgTime);
    for (uint32_t i = 0; i < numBuf; i++) {
      printf("compression Buf time [%d]: %f seconds\n", i, compBufTime[i]);
      printf("totalCompressedSize[%d] %zu [%f]\n", i, totalCompressedSize[i],
             totalCompressedSize[i] * 1.0 / bufSize);
    }
    printf("compression C time: %f seconds\n", compressTime);
  }
  return result;
  */
}

// Python callable function to combine four buffers into a single bytearray
PyObject *py_combine_dtype16(PyObject *self, PyObject *args) {
  Py_buffer view;

  int bits_mode, bytes_mode, threads;
  uint32_t numBuf = 2;
  uint32_t chunkSize = 128 * 1024;  // TBD

  if (!PyArg_ParseTuple(args, "y*iii", &view, &bits_mode, &bytes_mode,
                        &threads)) {
    return NULL;
  }

  clock_t startTime, endTime;
  uint32_t header_offset = 0;
  uint32_t compChunksSize_offset[] = {0, 0};
  uint32_t data_offset[] = {0, 0};

  startTime = clock();
  u_int8_t *bufUint8Pointer = (u_int8_t *)view.buf;
  u_int8_t isBufComp[numBuf];
  memcpy(&isBufComp, bufUint8Pointer + header_offset, numBuf * sizeof(u_int8_t));
  header_offset += numBuf * sizeof(u_int8_t);
  size_t origSize;
  memcpy(&origSize, bufUint8Pointer + header_offset, sizeof(size_t));
  header_offset += sizeof(size_t);
  size_t bufSize[numBuf];
  memcpy(&bufSize, bufUint8Pointer + header_offset, numBuf * sizeof(size_t));
  header_offset += numBuf * sizeof(size_t);
  size_t numChunks;
  memcpy(&numChunks, bufUint8Pointer + header_offset, sizeof(size_t));
  header_offset += sizeof(size_t);
  size_t compChunksSize[numBuf][numChunks];
  size_t decompressedSize[numBuf];
  u_int8_t *decompressedData[] = {NULL, NULL};
  size_t offset_compChunksSize = header_offset;

  // calc offset for the compChunksSize and offest to the data
  compChunksSize_offset[0] = header_offset;
  compChunksSize_offset[1] = header_offset;
  data_offset[0] = header_offset;

  if (isBufComp[0]) {
    compChunksSize_offset[1] += numChunks * sizeof(size_t);
    data_offset[0] += numChunks * sizeof(size_t);
  }
  if (isBufComp[1]) {
    data_offset[0] += numChunks * sizeof(size_t);
  }

  data_offset[1] = data_offset[0];

  for (uint32_t i = 0; i < numBuf; i++) {
    if (isBufComp[i]) {
      decompressedData[i] = PyMem_Malloc(origSize / numBuf);
      if (!decompressedData[i]) {
        PyErr_SetString(PyExc_MemoryError, "Failed to allocate memory");
        for (uint32_t j = 0; j < i; j++) {
          if (isBufComp[j]) {
            free(decompressedData[j]);
          }
        }
      }
      memcpy(compChunksSize[i],
             bufUint8Pointer + compChunksSize_offset[i],
             numChunks * sizeof(size_t));

      decompressedSize[i] = hufDecompressData(
          bufUint8Pointer + data_offset[i], compChunksSize[i], numChunks,
          origSize / numBuf, decompressedData[i], chunkSize);

      if (i < numBuf - 1) {
        data_offset[i + 1] += compChunksSize[i][numChunks - 1];
      }

    } else {
      decompressedData[i] = bufUint8Pointer + data_offset[0];
      if (i < numBuf - 1) {
        data_offset[i + 1] += origSize / numBuf;
      }
    }
  }
  /*
  endTime = clock();
  double decompressTime = (double)(endTime - startTime) / CLOCKS_PER_SEC;
  printf("decompression C time: %f seconds\n", decompressTime);
  printf ("buf len %zu\n " , view.len);
  printf ("bits_mode %d\n " , bits_mode);
  printf ("bytes_mode %d\n " , bytes_mode);
  printf ("threads %d\n " , threads);
  printf ("isBufComp[0] %d\n " , isBufComp[0]);
  printf ("isBufComp[1] %d\n " , isBufComp[1]);
  printf ("Original size %zu\n " , origSize);
  printf ("BufLen1 %zu\n " , bufSize[0]);
  printf ("BufLen2 %zu\n " , bufSize[1]);
  printf ("numChunks %zu\n " , numChunks);
  printf ("compChunksSize[1][0] %zu\n " , compChunksSize[1][0]);
  printf ("compChunksSize[1][1] %zu\n " , compChunksSize[1][1]);
  printf ("compChunksSize[1][numChunks-1] %zu\n " ,
  compChunksSize[1][numChunks-1]); printf ("decompressedSize[1] %zu\n",
  decompressedSize[1]);
  */
  u_int8_t *result = combine_buffers((u_int8_t *)decompressedData[0],
                                    (u_int8_t *)decompressedData[1],
                                    origSize / 2, bytes_mode, threads);
  if (result == NULL) {
    PyBuffer_Release(&view);
    PyErr_SetString(PyExc_MemoryError, "Failed to allocate memory");
    return NULL;
  }

  // Revert the reordering of all floats if needed
  if (bits_mode == 1) {
    revert_all_floats(result, origSize);
  }
  PyObject *py_result =
      PyByteArray_FromStringAndSize((const char *)result, origSize);
  for (uint32_t i = 0; i < numBuf; i++) {
    if (isBufComp[i]) {
      free(decompressedData[i]);
    }
  }

  PyMem_Free(result);
  PyBuffer_Release(&view);

  return py_result;
}
