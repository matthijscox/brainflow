#ifndef DATABUFFER
#define DATABUFFER

#include <SpinLock.h>


class DataBuffer 
{

		SpinLock lock;

		double *timestamps;
		float *data;
		
		size_t buffer_size;
		size_t first_used, first_free;
		size_t count;
		size_t num_channels;

		size_t next (size_t index) {
			return (index + 1) % buffer_size;
		}

		void get_chunk (size_t start, size_t size, double *tsBuf, float *data_buf);

	public:

		DataBuffer (size_t num_channels, size_t buffer_size);
		~DataBuffer ();

		void add_data (double timestamp, float *values);
		size_t get_data (size_t max_count, double *ts_buf, float *data_buf);
		size_t get_current_data (size_t max_count, double *ts_buf, float *data_buf);
		size_t get_data_count ();

};

#endif
