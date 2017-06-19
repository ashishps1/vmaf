/**
 *
 *  Copyright 2016-2017 Netflix, Inc.
 *
 *     Licensed under the Apache License, Version 2.0 (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

#include "common/alloc.h"
#include "common/file_io.h"
#include "motion_tools.h"
#include "common/convolution.h"
#include "common/convolution_internal.h"
#include "iqa/ssim_tools.h"
#include "darray.h"
#include "adm_options.h"

typedef float number_t;

#define read_image_b       read_image_b2s
#define read_image_w       read_image_w2s
#define convolution_f32_c  convolution_f32_c_s
#define offset_image       offset_image_s
#define FILTER_5           FILTER_5_s
int compute_adm(const float *ref, const float *dis, int w, int h, int ref_stride, int dis_stride, double *score, double *score_num, double *score_den, double *scores, double border_factor);
#ifdef COMPUTE_ANSNR
int compute_ansnr(const float *ref, const float *dis, int w, int h, int ref_stride, int dis_stride, double *score, double *score_psnr, double peak, double psnr_max);
#endif
int compute_vif(const float *ref, const float *dis, int w, int h, int ref_stride, int dis_stride, double *score, double *score_num, double *score_den, double *scores);
int compute_motion(const float *ref, const float *dis, int w, int h, int ref_stride, int dis_stride, double *score);
int compute_psnr(const float *ref, const float *dis, int w, int h, int ref_stride, int dis_stride, double *score, double peak, double psnr_max);
int compute_ssim(const number_t *ref, const number_t *cmp, int w, int h, int ref_stride, int cmp_stride, double *score, double *l_score, double *c_score, double *s_score);
int compute_ms_ssim(const number_t *ref, const number_t *cmp, int w, int h, int ref_stride, int cmp_stride, double *score, double* l_scores, double* c_scores, double* s_scores);

int combo1(int (*read_frame)(float *ref_data, int *ref_stride, float *main_data, int *main_stride, double *score), int w, int h, const char *fmt,
        DArray *adm_num_array,
        DArray *adm_den_array,
        DArray *adm_num_scale0_array,
        DArray *adm_den_scale0_array,
        DArray *adm_num_scale1_array,
        DArray *adm_den_scale1_array,
        DArray *adm_num_scale2_array,
        DArray *adm_den_scale2_array,
        DArray *adm_num_scale3_array,
        DArray *adm_den_scale3_array,
        DArray *motion_array,
        DArray *vif_num_scale0_array,
        DArray *vif_den_scale0_array,
        DArray *vif_num_scale1_array,
        DArray *vif_den_scale1_array,
        DArray *vif_num_scale2_array,
        DArray *vif_den_scale2_array,
        DArray *vif_num_scale3_array,
        DArray *vif_den_scale3_array,
        DArray *vif_array,
        DArray *psnr_array,
        DArray *ssim_array,
        DArray *ms_ssim_array,
        char *errmsg
        )
{
    double score = 0;
    double scores[4*2];
    double score_num = 0;
    double score_den = 0;
    double l_score = 0, c_score = 0, s_score = 0;
    double l_scores[SCALES], c_scores[SCALES], s_scores[SCALES];


    double score_psnr = 0;

    number_t *ref_buf = 0;
    number_t *main_buf = 0;
    number_t *prev_blur_buf = 0;
    number_t *blur_buf = 0;
    number_t *temp_buf = 0;

    size_t data_sz;
	
    int ref_stride=384, main_stride=384;
	int ret,cru=0;
	//printf("before reading first frame\n");
	
	data_sz = (sizeof(float))*ref_stride * h;

	ref_buf = aligned_malloc(data_sz, MAX_ALIGN);	
	main_buf = aligned_malloc(data_sz, MAX_ALIGN);

	ret = read_frame(ref_buf, &ref_stride, main_buf, &main_stride, &score);
		
	//printf("after reading first frame\n");

    if (w <= 0 || h <= 0 || (size_t)w > ALIGN_FLOOR(INT_MAX) / sizeof(number_t))
    {
        sprintf(errmsg, "wrong width %d or height %d.\n", w, h);
        goto fail_or_end;
    }

    //stride = ALIGN_CEIL(w * sizeof(number_t));

    if ((size_t)h > SIZE_MAX / ref_stride)
    {
        sprintf(errmsg, "height %d too large.\n", h);
        goto fail_or_end;
    }

    // prev_blur_buf, blur_buf for motion only
    if (!(prev_blur_buf = aligned_malloc(data_sz, MAX_ALIGN)))
    {
        sprintf(errmsg, "aligned_malloc failed for prev_blur_buf.\n");
        goto fail_or_end;
    }
    if (!(blur_buf = aligned_malloc(data_sz, MAX_ALIGN)))
    {
        sprintf(errmsg, "aligned_malloc failed for blur_buf.\n");
        goto fail_or_end;
    }

    // use temp_buf for convolution_f32_c, and fread u and v
    if (!(temp_buf = aligned_malloc(data_sz * 2, MAX_ALIGN)))
    {
        sprintf(errmsg, "aligned_malloc failed for temp_buf.\n");
        goto fail_or_end;
    }

    size_t offset;
    if (!strcmp(fmt, "yuv420p") || !strcmp(fmt, "yuv420p10le"))
    {
        if ((w * h) % 2 != 0)
        {
            sprintf(errmsg, "(w * h) %% 2 != 0, w = %d, h = %d.\n", w, h);
            goto fail_or_end;
        }
        offset = w * h / 2;
    }
    else if (!strcmp(fmt, "yuv422p") || !strcmp(fmt, "yuv422p10le"))
    {
        offset = w * h;
    }
    else if (!strcmp(fmt, "yuv444p") || !strcmp(fmt, "yuv444p10le"))
    {
        offset = w * h * 2;
    }
    else
    {
        sprintf(errmsg, "unknown format %s.\n", fmt);
        goto fail_or_end;
    }

    int frm_idx = 0;
	
	//printf("%s\n",fmt);
    // read dis y
/*
    if (!strcmp(fmt, "yuv420p") || !strcmp(fmt, "yuv422p") || !strcmp(fmt, "yuv444p"))
    {
		printf("this format");
        ret = read_image_b(main_data, main_buf, 0, w, h, main_stride);
    }
    else if (!strcmp(fmt, "yuv420p10le") || !strcmp(fmt, "yuv422p10le") || !strcmp(fmt, "yuv444p10le"))
    {
		printf("this format1");
        ret = read_image_w(main_data, main_buf, 0, w, h, main_stride);
    }
    else
    {
        sprintf(errmsg, "unknown format %s.\n", fmt);
        goto fail_or_end;
    }
printf("after reading\n");

	
	// read ref y
    if (!strcmp(fmt, "yuv420p") || !strcmp(fmt, "yuv422p") || !strcmp(fmt, "yuv444p"))
    {
        ret = read_image_b(ref_data, ref_buf, 0, w, h, ref_stride);
		int i,j;		
		float *ptr = ref_buf; 
		printf("\n\n--------------------------------------------------------------------------------\n\n");
		for(i=0;i<h;i++){
			for(j=0;j<w;j++){
				printf("%f ",ptr[j]);
			}
			ptr += ref_stride;
			printf("\n");
		}		
    }
    else if (!strcmp(fmt, "yuv420p10le") || !strcmp(fmt, "yuv422p10le") || !strcmp(fmt, "yuv444p10le"))
    {
        ret = read_image_w(ref_data, ref_buf, 0, w, h, ref_stride);
    }
    else
    {
        sprintf(errmsg, "unknown format %s.\n", fmt);
        goto fail_or_end;
    }
*/
//printf("First frame read successfully\n");
/*
while(1)
{
	ret = compute_psnr(ref_buf, main_buf, w, h, ref_stride, main_stride, &score, 255.0, 60.0);
	printf("psnr: %.3f, \n", score);
	read_frame(ref_buf, &ref_stride, main_buf, &main_stride);
}
*/

    while (1)
    {

        if (ret == 1)
        {
            break;
        }

//printf("before psnr\n");
//        printf("frame: %d, ", frm_idx);
        // ===============================================================
        // for the PSNR, SSIM and MS-SSIM, offset are 0 - do them first
        // ===============================================================

        if (psnr_array != NULL)
        {
            if (!strcmp(fmt, "yuv420p") || !strcmp(fmt, "yuv422p") || !strcmp(fmt, "yuv444p"))
            {
                // max psnr 60.0 for 8-bit per Ioannis
                ret = compute_psnr(ref_buf, main_buf, w, h, ref_stride, main_stride, &score, 255.0, 60.0);
            }
            else if (!strcmp(fmt, "yuv420p10le") || !strcmp(fmt, "yuv422p10le") || !strcmp(fmt, "yuv444p10le"))
            {
                // 10 bit gets normalized to 8 bit, peak is 1023 / 4.0 = 255.75
                // max psnr 72.0 for 10-bit per Ioannis
                ret = compute_psnr(ref_buf, main_buf, w, h, ref_stride, main_stride, &score, 255.75, 72.0);
            }
            else
            {
                sprintf(errmsg, "unknown format %s.\n", fmt);
                goto fail_or_end;
            }
            if (ret)
            {
                sprintf(errmsg, "compute_psnr failed.\n");
                goto fail_or_end;
            }

//            printf("psnr: %.3f, \n", score);

            insert_array(psnr_array, score);
        }

//printf("after psnr\n");

//printf("before ssim\n");

        if (ssim_array != NULL)
        {
            if ((ret = compute_ssim(ref_buf, main_buf, w, h, ref_stride, main_stride, &score, &l_score, &c_score, &s_score)))
            {
                sprintf(errmsg, "compute_ssim failed.\n");
                goto fail_or_end;
            }


      //      printf("ssim: %.3f, ", score);


            insert_array(ssim_array, score);
        }
//printf("after ssim\n");

//printf("before ms_ssim\n");
        if (ms_ssim_array != NULL)
        {
         
            if ((ret = compute_ms_ssim(ref_buf, main_buf, w, h, ref_stride, main_stride, &score, l_scores, c_scores, s_scores)))
            {
                sprintf(errmsg, "compute_ms_ssim failed.\n");
                goto fail_or_end;
            }


//            printf("ms_ssim: %.3f, ", score);

            insert_array(ms_ssim_array, score);
        }
//printf("after ms_ssim\n");
        // ===============================================================
        // for the rest, offset pixel by OPT_RANGE_PIXEL_OFFSET
        // ===============================================================
//printf("before offset\n");
        offset_image(ref_buf, OPT_RANGE_PIXEL_OFFSET, w, h, ref_stride);
        offset_image(main_buf, OPT_RANGE_PIXEL_OFFSET, w, h, main_stride);
//printf("after offset\n");

//printf("before compute_adm\n");
        if ((ret = compute_adm(ref_buf, main_buf, w, h, ref_stride, main_stride, &score, &score_num, &score_den, scores, ADM_BORDER_FACTOR)))
        {
            sprintf(errmsg, "compute_adm failed.\n");
            goto fail_or_end;
        }
//printf("after compute_adm\n");

/*        printf("adm: %.3f, ", score);
        printf("adm_num: %.3f, ", score_num);
        printf("adm_den: %.3f, ", score_den);
        printf("adm_num_scale0: %.3f, ", scores[0]);
        printf("adm_den_scale0: %.3f, ", scores[1]);
        printf("adm_num_scale1: %.3f, ", scores[2]);
        printf("adm_den_scale1: %.3f, ", scores[3]);
        printf("adm_num_scale2: %.3f, ", scores[4]);
        printf("adm_den_scale2: %.3f, ", scores[5]);
        printf("adm_num_scale3: %.3f, ", scores[6]);
        printf("adm_den_scale3: %.3f, ", scores[7]);
*/
        insert_array(adm_num_array, score_num);
        insert_array(adm_den_array, score_den);
        insert_array(adm_num_scale0_array, scores[0]);
        insert_array(adm_den_scale0_array, scores[1]);
        insert_array(adm_num_scale1_array, scores[2]);
        insert_array(adm_den_scale1_array, scores[3]);
        insert_array(adm_num_scale2_array, scores[4]);
        insert_array(adm_den_scale2_array, scores[5]);
        insert_array(adm_num_scale3_array, scores[6]);
        insert_array(adm_den_scale3_array, scores[7]);


//printf("before compute_ansnr\n");
        if (!strcmp(fmt, "yuv420p") || !strcmp(fmt, "yuv422p") || !strcmp(fmt, "yuv444p"))
        {
            // max psnr 60.0 for 8-bit per Ioannis
            ret = compute_ansnr(ref_buf, main_buf, w, h, ref_stride, main_stride, &score, &score_psnr, 255.0, 60.0);
        }
        else if (!strcmp(fmt, "yuv420p10le") || !strcmp(fmt, "yuv422p10le") || !strcmp(fmt, "yuv444p10le"))
        {
            // 10 bit gets normalized to 8 bit, peak is 1023 / 4.0 = 255.75
            // max psnr 72.0 for 10-bit per Ioannis
            ret = compute_ansnr(ref_buf, main_buf, w, h, ref_stride, main_stride, &score, &score_psnr, 255.75, 72.0);
        }
        else
        {
            sprintf(errmsg, "unknown format %s.\n", fmt);
            goto fail_or_end;
        }
        if (ret)
        {
            sprintf(errmsg, "compute_ansnr failed.\n");
            goto fail_or_end;
        }
//printf("after compute_ansnr\n");

//        printf("ansnr: %.3f, ", score);
//        printf("anpsnr: %.3f, ", score_psnr);


        // filter
        // apply filtering (to eliminate effects film grain)
        // stride input to convolution_f32_c is in terms of (sizeof(number_t) bytes)
        // since stride = ALIGN_CEIL(w * sizeof(number_t)), stride divides sizeof(number_t)
        convolution_f32_c(FILTER_5, 5, ref_buf, blur_buf, temp_buf, w, h, ref_stride / sizeof(number_t), main_stride / sizeof(number_t));

        // compute
        if (frm_idx == 0)
        {
            score = 0.0;
        }
        else
        {
            if ((ret = compute_motion(prev_blur_buf, blur_buf, w, h, ref_stride, main_stride, &score)))
            {
                sprintf(errmsg, "compute_motion failed.\n");
                goto fail_or_end;
            }
        }

        // copy to prev_buf
        memcpy(prev_blur_buf, blur_buf, data_sz);


 //       printf("motion: %.3f, ", score);


        insert_array(motion_array, score);

     
        if ((ret = compute_vif(ref_buf, main_buf, w, h, ref_stride, main_stride, &score, &score_num, &score_den, scores)))
        {
            sprintf(errmsg, "compute_vif failed.\n");
            goto fail_or_end;
        }

        // printf("vif_num: %.3f, ", score_num);
        // printf("vif_den: %.3f, ", score_den);
/*        printf("vif_num_scale0: %.3f, ", scores[0]);
        printf("vif_den_scale0: %.3f, ", scores[1]);
        printf("vif_num_scale1: %.3f, ", scores[2]);
        printf("vif_den_scale1: %.3f, ", scores[3]);
        printf("vif_num_scale2: %.3f, ", scores[4]);
        printf("vif_den_scale2: %.3f, ", scores[5]);
        printf("vif_num_scale3: %.3f, ", scores[6]);
        printf("vif_den_scale3: %.3f, ", scores[7]);
        printf("vif: %.3f, ", score);
*/
        insert_array(vif_num_scale0_array, scores[0]);
        insert_array(vif_den_scale0_array, scores[1]);
        insert_array(vif_num_scale1_array, scores[2]);
        insert_array(vif_den_scale1_array, scores[3]);
        insert_array(vif_num_scale2_array, scores[4]);
        insert_array(vif_den_scale2_array, scores[5]);
        insert_array(vif_num_scale3_array, scores[6]);
        insert_array(vif_den_scale3_array, scores[7]);
        insert_array(vif_array, score);

		ret = read_frame(ref_buf, &ref_stride, main_buf, &main_stride, &score);

//        printf("\n");

/*
        // read dis y
        if (!strcmp(fmt, "yuv420p") || !strcmp(fmt, "yuv422p") || !strcmp(fmt, "yuv444p"))
        {
            ret = read_image_b(main_data, main_buf, 0, w, h, main_stride);
        }
        else if (!strcmp(fmt, "yuv420p10le") || !strcmp(fmt, "yuv422p10le") || !strcmp(fmt, "yuv444p10le"))
        {
            ret = read_image_w(main_data, main_buf, 0, w, h, main_stride);
        }
        else
        {
            sprintf(errmsg, "unknown format %s.\n", fmt);
            goto fail_or_end;
        }
        // read main y
        if (!strcmp(fmt, "yuv420p") || !strcmp(fmt, "yuv422p") || !strcmp(fmt, "yuv444p"))
        {
            ret = read_image_b(ref_data, ref_buf, 0, w, h, ref_stride);
        }
        else if (!strcmp(fmt, "yuv420p10le") || !strcmp(fmt, "yuv422p10le") || !strcmp(fmt, "yuv444p10le"))
        {
            ret = read_image_w(ref_data, ref_buf, 0, w, h, ref_stride);
        }
        else
        {
            sprintf(errmsg, "unknown format %s.\n", fmt);
            goto fail_or_end;
        }
*/
	//printf("Frame %d, Score=%f\n",frm_idx++,score);	
    }

    ret = 0;

fail_or_end:
    aligned_free(ref_buf);
    aligned_free(main_buf);

    aligned_free(prev_blur_buf);
    aligned_free(blur_buf);
    aligned_free(temp_buf);

    return ret;
}
