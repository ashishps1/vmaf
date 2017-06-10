#include "vmaf.h"
#include "vmaf_wrapper.h"
#include "cpu.h"
#include <cstdint>

enum vmaf_cpu cpu; // global

extern "C" {

	double RunVmaf1(char* fmt, int width, int height, int (*read_frame)(uint8_t *ref_buf, int *ref_stride, uint8_t *main_buf, int *main_stride), const char *model_path,
	           const char *log_path, const char *log_fmt,
	           int disable_clip, int enable_transform,
	           int do_psnr, int do_ssim, int do_ms_ssim,
	           const char *pool_method){   
		
		bool d_c = false;
		bool disable_avx = false;
		bool e_t = false;
		bool d_p = false;
		bool d_s = false;
		bool d_m_s = false;

		if(disable_clip){
			d_c = true;	
		}
		if(enable_transform){
			e_t = true;	
		}
		if(do_psnr){
			d_p = true;	
		}
		if(do_ssim){
			d_s = true;	
		}
		if(do_ms_ssim){
			d_m_s = true;	
		}
		
        cpu = cpu_autodetect();

        if (disable_avx)
        {
            cpu = VMAF_CPU_NONE;
        }

		try
		{
		    double score = RunVmaf(fmt, width, height, read_frame, model_path, log_path, log_fmt, d_c, e_t, d_p, d_s, d_m_s, pool_method);
		    return 0.0;
		}
		catch (const std::exception &e)
		{
		    fprintf(stderr, "Error: %s\n", e.what());
		    return -1;
		}
	

	}


}