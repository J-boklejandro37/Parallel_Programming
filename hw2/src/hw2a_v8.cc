// Dynamic load balancing with Class
// Splitting chunks inside the row
// CHUNK_SIZE 64 -> 94.48 s
// CHUNK_SIZE 128 -> 93.62 s
// CHUNK_SIZE 1024 -> 93.62 s
// CHUNK_SIZE 2048 -> 93.57 s
// CHUNK_SIZE 10000 (width) -> 93.58 s

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define PNG_NO_SETJMP
#include <assert.h>
#include <cstring>
#include <immintrin.h>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <png.h>
#include <sched.h>
#include <stdexcept>
#include <vector>

struct Chunk
{
    int row, x_start, x_span;
    Chunk(){}
    Chunk(int r, int xst, int xsp) : row(r), x_start(xst), x_span(xsp) {}
};

class TaskQueue
{
private:
    std::vector<Chunk> chunks;  // Store row numbers to process
    pthread_mutex_t mutex;
    size_t current_chunk;       // The iterator of the chunk
    const int CHUNK_SIZE = 2048;

public:
    TaskQueue(int height, int width) : current_chunk(0)
    {
        pthread_mutex_init(&mutex, nullptr);
        // Create tasks for each row
        chunks.reserve(height * (width + CHUNK_SIZE - 1) / CHUNK_SIZE);
        for (int row = 0; row < height; ++row)
        {
            for (int x_start = 0; x_start < width; x_start += CHUNK_SIZE)
            {
                int x_span = std::min(CHUNK_SIZE, width - x_start);
                chunks.emplace_back(row, x_start, x_span);
            }
        }
    }

    ~TaskQueue()
    {
        pthread_mutex_destroy(&mutex);
    }

    // Get next row to process. Returns -1 if no more rows.
    Chunk getNextChunk()
    {
        pthread_mutex_lock(&mutex);
        Chunk chunk = {-1, 0, 0};
        if (current_chunk < chunks.size())
        {
            chunk = chunks[current_chunk++];
        }
        pthread_mutex_unlock(&mutex);
        return std::move(chunk);
    }
};

class PNGWriter
{
private:
    std::string filename;
    int width;
    int height;
    int iterations;
    const int* buffer;
    
    void validateInputs() const
    {
        if (!buffer) throw std::runtime_error("Invalid buffer");
        if (width <= 0 || height <= 0) throw std::runtime_error("Invalid dimensions");
    }

public:
    PNGWriter(const std::string& fname, int iters, int w, int h, const int* buf)
        : filename(fname), width(w), height(h), iterations(iters), buffer(buf) 
    {
        validateInputs();
    }

    void write() const 
    {
        std::unique_ptr<FILE, decltype(&fclose)> fp(fopen(filename.c_str(), "wb"), fclose);
        if (!fp) throw std::runtime_error("Failed to open file: " + filename);

        png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png_ptr) throw std::runtime_error("Failed to create PNG write struct");

        png_infop info_ptr = png_create_info_struct(png_ptr);
        if (!info_ptr)
        {
            png_destroy_write_struct(&png_ptr, nullptr);
            throw std::runtime_error("Failed to create PNG info struct");
        }

        png_init_io(png_ptr, fp.get());
        png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_set_filter(png_ptr, 0, PNG_NO_FILTERS);
        png_write_info(png_ptr, info_ptr);
        png_set_compression_level(png_ptr, 1);

        std::unique_ptr<png_byte[]> row = std::make_unique<png_byte[]>(3 * width);
        const size_t row_size = 3 * width * sizeof(png_byte);

        for (int y = 0; y < height; ++y)
        {
            std::memset(row.get(), 0, row_size);
            for (int x = 0; x < width; ++x)
            {
                int p = buffer[(height - 1 - y) * width + x];
                png_bytep color = row.get() + x * 3;
                if (p != iterations)
                {
                    if (p & 16)
                    {
                        color[0] = 240;
                        color[1] = color[2] = (p & 15) << 4;
                    }
                    else color[0] = (p & 15) << 4;
                }
            }
            png_write_row(png_ptr, row.get());
        }

        png_write_end(png_ptr, nullptr);
        png_destroy_write_struct(&png_ptr, &info_ptr);
    }
};

class MandelbrotGenerator
{
private:
    double left, right, lower, upper;
    int width, height, iters, thread_num;
    std::unique_ptr<int[]> image;
    std::unique_ptr<TaskQueue> task_queue;

    struct ThreadData
    {
        MandelbrotGenerator* obj;
        int thread_id; // thread_id is not used
    };

    // need to use "static" so that the type is actually void* (*)(void*), or it will be void* (MandelbrotGenerator::*)(void*), meaning a "member function pointer"
    static void* wrapper(void* arg)
    {
        ThreadData* data = static_cast<ThreadData*>(arg);
        data->obj->compute(data->thread_id); // thread_id is not used
        return nullptr;
    }

    void compute(int thread_id) // thread_id is not used
    {
        double x_offset = (right - left) / width;
        double y_offset = (upper - lower) / height;

        // Constants for vectorized computation
        __m512d vec_two = _mm512_set1_pd(2.0);
        __m512d vec_four = _mm512_set1_pd(4.0);
        __m512d vec_x_offset = _mm512_set1_pd(x_offset);
        __m512d vec_left = _mm512_set1_pd(left);

        // Process rows dynamically
        while (true)
        {
            auto chunk = task_queue->getNextChunk();
            if (chunk.row == -1) break; // No more chunks to process

            int row = chunk.row;
            int x_start = chunk.x_start;
            int x_span = chunk.x_span;

            // double y0 = row * y_offset + lower;
            // for (int i = 0; i < x_span; ++i)
            // {
            //     double x0 = (x_start + i) * x_offset + left;

            //     int repeats = 0;
            //     double x = 0;
            //     double y = 0;
            //     double length_squared = 0;
            //     while (repeats < iters && length_squared < 4)
            //     {
            //         double temp = x * x - y * y + x0;
            //         y = 2 * x * y + y0;
            //         x = temp;
            //         length_squared = x * x + y * y;
            //         ++repeats;
            //     }
            //     image[row * width + x_start + i] = repeats;
            // }

            double y0 = row * y_offset + lower;
            __m512d vec_y0 = _mm512_set1_pd(y0);
            __m512d vec_x_start = _mm512_set1_pd(x_start);

            int i;
            for (i = 0; i < x_span - 7; i += 8)
            {
                // Each x0 = (x_start + i) * x_offset + left
                __m512d vec_x0 = _mm512_fmadd_pd(
                    _mm512_add_pd(vec_x_start, _mm512_set_pd(i+7, i+6, i+5, i+4, i+3, i+2, i+1, i)),
                    vec_x_offset,
                    vec_left
                );

                // Initialize vectors
                __m512d vec_x = _mm512_setzero_pd();
                __m512d vec_y = _mm512_setzero_pd();
                __m512d vec_x2 = _mm512_setzero_pd();
                __m512d vec_y2 = _mm512_setzero_pd();
                __m512d vec_length_squared = _mm512_setzero_pd();
                __m256i vec_repeats = _mm256_setzero_si256(); //  For storing back. *image is a 32-bit integer, meaning 256 / 32 = 8 integers 
                __mmask8 mask = 0xFF; // All 8 bits set
                
                for (int iter = 0; iter < iters; ++iter)
                {
                    vec_x2 = _mm512_mul_pd(vec_x, vec_x);
                    vec_y2 = _mm512_mul_pd(vec_y, vec_y);

                    // Length_squared = x^2 + y^2
                    vec_length_squared = _mm512_add_pd(vec_x2, vec_y2);
                    
                    // Check which elements are still within bound. => If length_sqared < 4: set mask bit = 1
                    mask = _mm512_cmp_pd_mask(vec_length_squared, vec_four, _CMP_LT_OS);
                    if (!mask) break;

                    // Calculate 2xy
                    __m512d vec_2xy = _mm512_mul_pd(_mm512_mul_pd(vec_x, vec_y), vec_two);

                    // New x = x^2 - y^2 + x0
                    vec_x = _mm512_add_pd(_mm512_sub_pd(vec_x2, vec_y2), vec_x0);

                    // New y = 2xy + y0
                    vec_y = _mm512_add_pd(vec_2xy, vec_y0);

                    // Only increment repeats for elements still within bounds
                    vec_repeats = _mm256_mask_add_epi32(
                        vec_repeats,            // src: values to keep if mask == 0
                        mask,                   // mask: elements to operate on
                        vec_repeats,            // a: first operand
                        _mm256_set1_epi32(1)    // b: second operand
                    );
                }
                
                // Store directly to image array
                // Endianess is automatically addressed, so the rightmost will be put first
                // _mm256_storeu_si256((__m256i*)&image[j * width + i], vec_repeats); // Traditional method, needs type casting
                _mm256_storeu_epi32(&image[row * width + x_start + i], vec_repeats); // Newer method, don't need type casting
            }
            // Finish the remaining
            for (; i < x_span; ++i)
            {
                double x0 = (x_start + i) * x_offset + left;
				double x = 0;
				double y = 0;
				double length_squared = 0;
				int repeats = 0;
				while (repeats < iters && length_squared < 4)
				{
					double temp = x * x - y * y + x0;
					y = 2 * x * y + y0;
					x = temp;
					length_squared = x * x + y * y;
					++repeats;
				}
				image[row * width + x_start + i] = repeats;
            }
        }
    }

public:
    MandelbrotGenerator(double l, double r, double low, double up, int w, int h, int iters, int thread_num)
        : left(l), right(r), lower(low), upper(up), width(w), height(h), iters(iters), thread_num(thread_num)
    {
        image = std::make_unique<int[]>(width * height);
        task_queue = std::make_unique<TaskQueue>(height, width); // TaskQueue is a Class, which takes in height and width as parameter in contructor
    }

    void generate()
    {
        pthread_t threads[thread_num]; // for thread instances
        int tids[thread_num]; // for thread_id
        ThreadData* thread_data = new ThreadData[thread_num]; // for callback function's argument

        // create threads and compute
        for (int i = 0; i < thread_num; ++i)
        {
            tids[i] = i;
            thread_data[i] = {this, i}; // pass in "this" so that the static function "wrapper" can access the member function "compute"
            pthread_create(&threads[i], nullptr, wrapper, &thread_data[i]); // can only pass in static function as callback function
        }

        // wait for all
        for (int i = 0; i < thread_num; ++i)
        {
            pthread_join(threads[i], nullptr);
        }
    }

    void saveToPNG(const std::string& filename) const
    {
        PNGWriter writer(filename, iters, width, height, image.get());
        writer.write();
    }
};

int main(int argc, char** argv)
{
    try
    {   // Detect available CPUs
        cpu_set_t cpu_set;
        sched_getaffinity(0, sizeof(cpu_set), &cpu_set);
        int thread_num = CPU_COUNT(&cpu_set);
        std::cout << thread_num << " cpus available\n";

        // Validate arguments
        if (argc != 9) throw std::runtime_error("Invalid number of arguments");

        // Parse arguments
        const std::string filename = argv[1];
        int iterations = std::stoi(argv[2]);
        double left = std::stod(argv[3]);
        double right = std::stod(argv[4]);
        double lower = std::stod(argv[5]);
        double upper = std::stod(argv[6]);
        int width = std::stoi(argv[7]);
        int height = std::stoi(argv[8]);

        // Generate and save Mandelbrot set
        MandelbrotGenerator mandelbrot(left, right, lower, upper, width, height, iterations, thread_num);
        mandelbrot.generate();
        mandelbrot.saveToPNG(filename);

        return 0;
    } 
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}