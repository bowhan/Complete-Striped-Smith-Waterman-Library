/*
 *  ssw.c
 *
 *  Created by Mengyao Zhao on 6/22/10.
 *  Copyright 2010 Boston College. All rights reserved.
 *	Version 0.1.4
 *	Last revision by Mengyao Zhao on 02/03/12.
 *	New features: Weight matrix is extracted. 
 *
 */

#include <emmintrin.h>
#include <stdint.h>
#include "ssw.h"

#ifdef __GNUC__
#define LIKELY(x) __builtin_expect((x),1)
#define UNLIKELY(x) __builtin_expect((x),0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

/* Generate query profile rearrange query sequence & calculate the weight of match/mismatch. */
__m128i* qP_byte (const char* read,
								   int8_t* nt_table,
								   int8_t* mat,
								   int32_t n,	/* the edge length of the squre matrix mat */
								   uint8_t bias) { 
					
	int32_t readLen = strlen(read);
	int32_t
	segLen = (readLen + 15) / 16; /* Split the 128 bit register into 16 pieces. 
								     Each piece is 8 bit. Split the read into 16 segments. 
								     Calculat 16 segments in parallel.
								   */
	__m128i* vProfile = (__m128i*)calloc(n * segLen, sizeof(__m128i));
	int8_t* t = (int8_t*)vProfile;
	int32_t nt, i, j;
	int32_t segNum;
	
	/* Generate query profile rearrange query sequence & calculate the weight of match/mismatch */
	for (nt = 0; LIKELY(nt < n); nt ++) {
		for (i = 0; i < segLen; i ++) {
			j = i; 
			for (segNum = 0; LIKELY(segNum < 16) ; segNum ++) {
				*t++ = j>= readLen ? 0 : mat[nt * n + nt_table[(int)read[j]]] + bias;
				j += segLen;
			}
		}
	}

	return vProfile;
}

/* Striped Smith-Waterman
   Record the highest score of each reference position. 
   Return the alignment score and ending position of the best alignment, 2nd best alignment, etc. 
   Gap begin and gap extension are different. 
   wight_match > 0, all other weights < 0.
   The returned positions are 0-based.
 */ 
alignment_end* sw_sse2_byte (const char* ref,
									int8_t* nt_table,
									int32_t refLen,
								    int32_t readLen, 
								    uint8_t weight_insertB, /* will be used as - */
								    uint8_t weight_insertE, /* will be used as - */
								    uint8_t weight_deletB,  /* will be used as - */
								    uint8_t weight_deletE,  /* will be used as - */
								    __m128i* vProfile,
									uint8_t terminate,	/* the best alignment score: used to terminate 
														   the matrix calculation when locating the 
														   alignment beginning point. If this score 
														   is set to 0, it will not be used */
	 							    uint8_t bias) {         /* Shift 0 point to a positive value. */

#define max16(m, vm) (vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 8)); \
					  (vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 4)); \
					  (vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 2)); \
					  (vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 1)); \
					  (m) = _mm_extract_epi16((vm), 0)
	
	uint8_t max = 0;		                     /* the max alignment score */
	int32_t end_read = 0;
	int32_t end_ref = 0; /* 1_based best alignment ending point; Initialized as isn't aligned - 0. */
	int32_t segLen = (readLen + 15) / 16; /* number of segment */
	
	/* array to record the largest score of each reference position */
	uint8_t* maxColumn = (uint8_t*) calloc(refLen, 1); 
	
	/* array to record the alignment read ending position of the largest score of each reference position */
	int32_t* end_read_column = (int32_t*) calloc(refLen, sizeof(int32_t));
	
	/* Define 16 byte 0 vector. */
	__m128i vZero = _mm_set1_epi32(0);

	__m128i* pvHStore = (__m128i*) calloc(segLen, sizeof(__m128i));
	__m128i* pvHLoad = (__m128i*) calloc(segLen, sizeof(__m128i));
	__m128i* pvE = (__m128i*) calloc(segLen, sizeof(__m128i));
	__m128i* pvHmax = (__m128i*) calloc(segLen, sizeof(__m128i));

	int32_t i, j, k;
	for (i = 0; LIKELY(i < segLen); i ++) {
		pvHStore[i] = vZero;
		pvE[i] = vZero;
	}
	
	/* 16 byte insertion begin vector */
	__m128i vInserB = _mm_set1_epi8(weight_insertB);
	
	/* 16 byte insertion extension vector */
	__m128i vInserE = _mm_set1_epi8(weight_insertE);	
	
	/* 16 byte deletion begin vector */
	__m128i vDeletB = _mm_set1_epi8(weight_deletB);	

	/* 16 byte deletion extension vector */
	__m128i vDeletE = _mm_set1_epi8(weight_deletE);	

	/* 16 byte bias vector */
	__m128i vBias = _mm_set1_epi8(bias);	

	__m128i vMaxScore = vZero; /* Trace the highest score of the whole SW matrix. */
	__m128i vMaxMark = vZero; /* Trace the highest score till the previous column. */	
	__m128i vTemp;
	int32_t edge;
	/* outer loop to process the reference sequence */
	for (i = 0; LIKELY(i < refLen); i ++) {
		
		int32_t cmp;
		__m128i vF = vZero; /* Initialize F value to 0. 
							   Any errors to vH values will be corrected in the Lazy_F loop. 
							 */
		__m128i vH = pvHStore[segLen - 1];
		vH = _mm_slli_si128 (vH, 1); /* Shift the 128-bit value in vH left by 1 byte. */
		
		/* Swap the 2 H buffers. */
		__m128i* pv = pvHLoad;
		
		__m128i vMaxColumn = vZero; /* vMaxColumn is used to record the max values of column i. */
		
		__m128i* vP = vProfile + nt_table[(int)ref[i]] * segLen; /* Right part of the vProfile */
		pvHLoad = pvHStore;
		pvHStore = pv;
		
		/* inner loop to process the query sequence */
		for (j = 0; LIKELY(j < segLen); j ++) {

			vH = _mm_adds_epu8(vH, vP[j]);
			vH = _mm_subs_epu8(vH, vBias); /* vH will be always > 0 */

			/* Get max from vH, vE and vF. */
			vH = _mm_max_epu8(vH, pvE[j]);
			vH = _mm_max_epu8(vH, vF);
			
			/* Update highest score encountered this far. */
			vMaxScore = _mm_max_epu8(vMaxScore, vH);
			vMaxColumn = _mm_max_epu8(vMaxColumn, vH);
			
			/* Save vH values. */
			pvHStore[j] = vH;

			/* Update vE value. */
			vH = _mm_subs_epu8(vH, vInserB); /* saturation arithmetic, result >= 0 */
			pvE[j] = _mm_subs_epu8(pvE[j], vInserE);
			pvE[j] = _mm_max_epu8(pvE[j], vH);
			
			/* Update vF value. */
			vF = _mm_subs_epu8(vF, vDeletE);
			vF = _mm_max_epu8(vF, vH);
			
			/* Load the next vH. */
			vH = pvHLoad[j];
		}

		/* Lazy_F loop: has been revised to disallow adjecent insertion and then deletion, so don't update E(i, j), learn from SWPS3 */
		for (k = 0; LIKELY(k < 16); ++k) {
			vF = _mm_slli_si128 (vF, 1);
			for (j = 0; LIKELY(j < segLen); ++j) {
				pvHStore[j] = _mm_max_epu8(pvHStore[j], vF);
				vH = _mm_subs_epu8(pvHStore[j], vDeletB);
				vF = _mm_subs_epu8(vF, vDeletE);
				if (UNLIKELY(! _mm_movemask_epi8(_mm_cmpgt_epi8(vF, vH)))) goto end;
			}
		}

end:		
		vTemp = _mm_cmpeq_epi8(vMaxMark, vMaxScore);
		cmp = _mm_movemask_epi8(vTemp);
		if (cmp != 0xffff) {
			uint8_t temp; 
			vMaxMark = vMaxScore;
			max16(temp, vMaxScore);
			vMaxScore = vMaxMark;
			
			if (LIKELY(temp > max)) {
				max = temp;
				if (max + bias >= 255) {
					break;	//overflow
				}
				end_ref = i;
			
				/* Store the column with the highest alignment score in order to trace the alignment ending position on read. */
				for (j = 0; LIKELY(j < segLen); ++j) // keep the H1 vector
					pvHmax[j] = pvHStore[j];
			}
		}
		
		/* Record the max score of current column. */	
		max16(maxColumn[i], vMaxColumn);
		if (terminate > 0 && maxColumn[i] == terminate) break;
	} 	

	/* Trace the alignment ending position on read. */
	uint8_t *t = (uint8_t*)pvHmax;
	int32_t column_len = segLen * 16;
	for (i = 0; LIKELY(i < column_len); ++i, ++t) {
		if (*t == max) {
			end_read = i / 16 + i % 16 * segLen;
			break;
		}
	}

	/* Find the most possible 2nd best alignment. */
	alignment_end* bests = (alignment_end*) calloc(2, sizeof(alignment_end));
	bests[0].score = max + bias >= 255 ? 255 : max;
	bests[0].ref = end_ref;
	bests[0].read = end_read;
	
	bests[1].score = 0;
	bests[1].ref = 0;
	bests[1].read = 0;

	edge = (end_ref - readLen / 2 - 1) > 0 ? (end_ref - readLen / 2 - 1) : 0;
	for (i = 0; i < edge; i ++) {
		if (maxColumn[i] > bests[1].score) 
			bests[1].score = maxColumn[i];
	}
	edge = (end_ref + readLen / 2 + 1) > refLen ? refLen : (end_ref + readLen / 2 + 1);
	for (i = edge; i < refLen; i ++) {
		if (maxColumn[i] > bests[1].score) 
			bests[1].score = maxColumn[i];
	}
	
	free(pvHStore);
	free(maxColumn);
	free(end_read_column);
	return bests;
}

__m128i* qP_word (const char* read,
								   int8_t* nt_table,
								   int8_t* mat,
								   int32_t n) { 
					
	int32_t readLen = strlen(read);
	int32_t
	segLen = (readLen + 7) / 8; 
	__m128i* vProfile = (__m128i*)calloc(n * segLen, sizeof(__m128i));
	int16_t* t = (int16_t*)vProfile;
	int32_t nt, i, j;
	int32_t segNum;
	
	/* Generate query profile rearrange query sequence & calculate the weight of match/mismatch */
	for (nt = 0; LIKELY(nt < n); nt ++) {
		for (i = 0; i < segLen; i ++) {
			j = i; 
			for (segNum = 0; LIKELY(segNum < 8) ; segNum ++) {
				*t++ = j>= readLen ? 0 : mat[nt * n + nt_table[(int)read[j]]];
				j += segLen;
			}
		}
	}

	return vProfile;
}

alignment_end* sw_sse2_word (const char* ref,
									int8_t* nt_table,
									int32_t refLen,
								    int32_t readLen, 
								    uint8_t weight_insertB, /* will be used as - */
								    uint8_t weight_insertE, /* will be used as - */
								    uint8_t weight_deletB,  /* will be used as - */
								    uint8_t weight_deletE,  /* will be used as - */
								    __m128i* vProfile,
									uint16_t terminate) { 

#define max8(m, vm) (vm) = _mm_max_epi16((vm), _mm_srli_si128((vm), 8)); \
					(vm) = _mm_max_epi16((vm), _mm_srli_si128((vm), 4)); \
					(vm) = _mm_max_epi16((vm), _mm_srli_si128((vm), 2)); \
					(m) = _mm_extract_epi16((vm), 0)
	
	uint16_t max = 0;		                     /* the max alignment score */
	int32_t end_read = 0;
	int32_t end_ref = 0; /* 1_based best alignment ending point; Initialized as isn't aligned - 0. */
	int32_t segLen = (readLen + 7) / 8; /* number of segment */
	
	/* array to record the largest score of each reference position */
	uint16_t* maxColumn = (uint16_t*) calloc(refLen, 2); 
	
	/* array to record the alignment read ending position of the largest score of each reference position */
	int32_t* end_read_column = (int32_t*) calloc(refLen, sizeof(int32_t));
	
	/* Define 16 byte 0 vector. */
	__m128i vZero = _mm_set1_epi32(0);

	__m128i* pvHStore = (__m128i*) calloc(segLen, sizeof(__m128i));
	__m128i* pvHLoad = (__m128i*) calloc(segLen, sizeof(__m128i));
	__m128i* pvE = (__m128i*) calloc(segLen, sizeof(__m128i));
	__m128i* pvHmax = (__m128i*) calloc(segLen, sizeof(__m128i));

	int32_t i, j, k;
	for (i = 0; LIKELY(i < segLen); i ++) {
		pvHStore[i] = vZero;
		pvE[i] = vZero;
	}
	
	/* 16 byte insertion begin vector */
	__m128i vInserB = _mm_set1_epi16(weight_insertB);
	
	/* 16 byte insertion extension vector */
	__m128i vInserE = _mm_set1_epi16(weight_insertE);	
	
	/* 16 byte deletion begin vector */
	__m128i vDeletB = _mm_set1_epi16(weight_deletB);

	/* 16 byte deletion extension vector */
	__m128i vDeletE = _mm_set1_epi16(weight_deletE);	

	/* 16 byte bias vector */

	__m128i vMaxScore = vZero; /* Trace the highest score of the whole SW matrix. */
	__m128i vMaxMark = vZero; /* Trace the highest score till the previous column. */	
	__m128i vTemp;
	int32_t edge;
	/* outer loop to process the reference sequence */
	for (i = 0; LIKELY(i < refLen); i ++) {

		int32_t cmp;
		__m128i vF = vZero; /* Initialize F value to 0. 
							   Any errors to vH values will be corrected in the Lazy_F loop. 
							 */
		__m128i vH = pvHStore[segLen - 1];
		vH = _mm_slli_si128 (vH, 2); /* Shift the 128-bit value in vH left by 2 byte. */
		
		/* Swap the 2 H buffers. */
		__m128i* pv = pvHLoad;
		
		__m128i vMaxColumn = vZero; /* vMaxColumn is used to record the max values of column i. */
		
		__m128i* vP = vProfile + nt_table[(int)ref[i]] * segLen; /* Right part of the vProfile */
		pvHLoad = pvHStore;
		pvHStore = pv;
		
		/* inner loop to process the query sequence */
		for (j = 0; LIKELY(j < segLen); j ++) {
			vH = _mm_adds_epi16(vH, vP[j]);

			/* Get max from vH, vE and vF. */
			vH = _mm_max_epi16(vH, pvE[j]);
			vH = _mm_max_epi16(vH, vF);
			
			/* Update highest score encountered this far. */
			vMaxScore = _mm_max_epi16(vMaxScore, vH);
			vMaxColumn = _mm_max_epi16(vMaxColumn, vH);
			
			/* Save vH values. */
			pvHStore[j] = vH;

			/* Update vE value. */
			vH = _mm_subs_epu16(vH, vInserB); /* saturation arithmetic, result >= 0 */

			pvE[j] = _mm_subs_epu16(pvE[j], vInserE);
			pvE[j] = _mm_max_epi16(pvE[j], vH);

			/* Update vF value. */
			vF = _mm_subs_epu16(vF, vDeletE);
			vF = _mm_max_epi16(vF, vH);
			
			/* Load the next vH. */
			vH = pvHLoad[j];
		}

		/* Lazy_F loop: has been revised to disallow adjecent insertion and then deletion, so don't update E(i, j), learn from SWPS3 */
		for (k = 0; LIKELY(k < 8); ++k) {
			vF = _mm_slli_si128 (vF, 2);
			for (j = 0; LIKELY(j < segLen); ++j) {
				pvHStore[j] = _mm_max_epi16(pvHStore[j], vF);
				vH = _mm_subs_epu16(pvHStore[j], vDeletB);
				vF = _mm_subs_epu16(vF, vDeletE);
				if (UNLIKELY(! _mm_movemask_epi8(_mm_cmpgt_epi16(vF, vH)))) goto end;
			}
		}

end:		
		vTemp = _mm_cmpeq_epi16(vMaxMark, vMaxScore);
		cmp = _mm_movemask_epi8(vTemp);
		if (cmp != 0xffff) {
			uint16_t temp; 
			vMaxMark = vMaxScore;
			max8(temp, vMaxScore);
			vMaxScore = vMaxMark;
			
			if (LIKELY(temp > max)) {
				max = temp;
				end_ref = i;
				for (j = 0; LIKELY(j < segLen); ++j) // keep the H1 vector
					pvHmax[j] = pvHStore[j];
			}
		}
		
		/* Record the max score of current column. */	
		max8(maxColumn[i], vMaxColumn);
		if (terminate > 0 && maxColumn[i] == terminate) break;
	} 	

	/* Trace the alignment ending position on read. */
	uint16_t *t = (uint16_t*)pvHmax;
	int32_t column_len = segLen * 8;
	for (i = 0; LIKELY(i < column_len); ++i, ++t) {
		if (*t == max) {
			end_read = i / 8 + i % 8 * segLen;
			break;
		}
	}

	/* Find the most possible 2nd best alignment. */
	alignment_end* bests = (alignment_end*) calloc(2, sizeof(alignment_end));
	bests[0].score = max;
	bests[0].ref = end_ref;
	bests[0].read = end_read;
	
	bests[1].score = 0;
	bests[1].ref = 0;
	bests[1].read = 0;

	edge = (end_ref - readLen / 2 - 1) > 0 ? (end_ref - readLen / 2 - 1) : 0;
	for (i = 0; i < edge; i ++) {
		if (maxColumn[i] > bests[1].score) 
			bests[1].score = maxColumn[i];
	}
	edge = (end_ref + readLen / 2 + 1) > refLen ? refLen : (end_ref + readLen / 2 + 1);
	for (i = edge; i < refLen; i ++) {
		if (maxColumn[i] > bests[1].score) 
			bests[1].score = maxColumn[i];
	}
	
	free(pvHStore);
	free(maxColumn);
	free(end_read_column);
	return bests;
}

