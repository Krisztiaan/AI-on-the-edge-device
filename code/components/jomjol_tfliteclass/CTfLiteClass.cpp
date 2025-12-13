#include "CTfLiteClass.h"
#include "ClassLogFile.h"
#include "Helper.h"
#include "psram.h"
#include "esp_log.h"
#include "../../include/defines.h"

#include <sys/stat.h>
#include <miniz.h>
#include <stdint.h>
#include <string.h>

// #define DEBUG_DETAIL_ON


static const char *TAG = "TFLITE";

static bool ends_with(const std::string &s, const char *suffix)
{
    const size_t slen = s.size();
    const size_t tlen = strlen(suffix);
    return slen >= tlen && s.compare(slen - tlen, tlen, suffix) == 0;
}

static bool read_le_u16(FILE *f, uint16_t &out)
{
    uint8_t b[2];
    if (fread(b, 1, sizeof(b), f) != sizeof(b)) return false;
    out = (uint16_t)(b[0] | (uint16_t)b[1] << 8);
    return true;
}

static bool read_le_u32(FILE *f, uint32_t &out)
{
    uint8_t b[4];
    if (fread(b, 1, sizeof(b), f) != sizeof(b)) return false;
    out = (uint32_t)(b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
    return true;
}

static bool gzip_skip_header(FILE *f, long file_size, long &out_deflate_offset)
{
    if (file_size < 18) { // gzip header (10) + footer (8)
        return false;
    }

    uint8_t hdr[10];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;
    if (hdr[0] != 0x1f || hdr[1] != 0x8b) return false;
    if (hdr[2] != 8) return false; // deflate
    const uint8_t flg = hdr[3];

    // Skip optional fields
    if (flg & 0x04) { // FEXTRA
        uint16_t xlen = 0;
        if (!read_le_u16(f, xlen)) return false;
        if (fseek(f, (long)xlen, SEEK_CUR) != 0) return false;
    }
    if (flg & 0x08) { // FNAME
        int c;
        do {
            c = fgetc(f);
            if (c == EOF) return false;
        } while (c != 0);
    }
    if (flg & 0x10) { // FCOMMENT
        int c;
        do {
            c = fgetc(f);
            if (c == EOF) return false;
        } while (c != 0);
    }
    if (flg & 0x02) { // FHCRC
        if (fseek(f, 2, SEEK_CUR) != 0) return false;
    }

    long pos = ftell(f);
    if (pos < 0) return false;
    if (pos >= file_size - 8) return false; // must leave footer
    out_deflate_offset = pos;
    return true;
}

static bool gzip_get_isize(FILE *f, long file_size, uint32_t &out_isize)
{
    if (file_size < 18) return false;
    if (fseek(f, file_size - 4, SEEK_SET) != 0) return false;
    return read_le_u32(f, out_isize);
}


void CTfLiteClass::MakeStaticResolver()
{
  resolver.AddFullyConnected();
  resolver.AddReshape();
  resolver.AddSoftmax();
  resolver.AddConv2D();
  resolver.AddMaxPool2D();
  resolver.AddQuantize();
  resolver.AddMul();
  resolver.AddAdd();
  resolver.AddLeakyRelu();
  resolver.AddDequantize();
}


float CTfLiteClass::GetOutputValue(int nr)
{
    TfLiteTensor* output2 = this->interpreter->output(0);

    int numeroutput = output2->dims->data[1];
    if ((nr+1) > numeroutput)
      return -1000;

    return output2->data.f[nr];
}


int CTfLiteClass::GetClassFromImageBasis(CImageBasis *rs)
{
    if (!LoadInputImageBasis(rs))
      return -1000;

    Invoke();

    return GetOutClassification();
}


int CTfLiteClass::GetOutClassification(int _von, int _bis)
{
  TfLiteTensor* output2 = interpreter->output(0);

  float zw_max;
  float zw;
  int zw_class;

  if (output2 == NULL)
    return -1;

  int numeroutput = output2->dims->data[1];
  //ESP_LOGD(TAG, "number output neurons: %d", numeroutput);

  if (_bis == -1)
    _bis = numeroutput -1;

  if (_von == -1)
    _von = 0;

  if (_bis >= numeroutput)
  {
    ESP_LOGD(TAG, "NUMBER OF OUTPUT NEURONS does not match required classification!");
    return -1;
  }

  zw_max = output2->data.f[_von];
  zw_class = _von;
  for (int i = _von + 1; i <= _bis; ++i)
  {
    zw = output2->data.f[i];
    if (zw > zw_max)
    {
        zw_max = zw;
        zw_class = i;
    }
  }
  return (zw_class - _von);
}


void CTfLiteClass::GetInputDimension(bool silent = false)
{
  TfLiteTensor* input2 = this->interpreter->input(0);

  int numdim = input2->dims->size;
  if (!silent)  ESP_LOGD(TAG, "NumDimension: %d", numdim);

  int sizeofdim;
  for (int j = 0; j < numdim; ++j)
  {
    sizeofdim = input2->dims->data[j];
    if (!silent) ESP_LOGD(TAG, "SizeOfDimension %d: %d", j, sizeofdim);
    if (j == 1) im_height = sizeofdim;
    if (j == 2) im_width = sizeofdim;
    if (j == 3) im_channel = sizeofdim;
  }
}


int CTfLiteClass::ReadInputDimenstion(int _dim)
{
  if (_dim == 0)
    return im_width;
  if (_dim == 1)
    return im_height;
  if (_dim == 2)
    return im_channel;

  return -1;
}


int CTfLiteClass::GetAnzOutPut(bool silent)
{
  TfLiteTensor* output2 = this->interpreter->output(0);

  int numdim = output2->dims->size;
  if (!silent) ESP_LOGD(TAG, "NumDimension: %d", numdim);

  int sizeofdim;
  for (int j = 0; j < numdim; ++j)
  {
    sizeofdim = output2->dims->data[j];
    if (!silent) ESP_LOGD(TAG, "SizeOfDimension %d: %d", j, sizeofdim);
  }


  float fo;

  // Process the inference results.
  int numeroutput = output2->dims->data[1];
  for (int i = 0; i < numeroutput; ++i)
  {
   fo = output2->data.f[i];
    if (!silent) ESP_LOGD(TAG, "Result %d: %f", i, fo);
  }
  return numeroutput;
}


void CTfLiteClass::Invoke()
{
    if (interpreter != nullptr)
      interpreter->Invoke();
}


bool CTfLiteClass::LoadInputImageBasis(CImageBasis *rs)
{
    #ifdef DEBUG_DETAIL_ON 
        LogFile.WriteHeapInfo("CTfLiteClass::LoadInputImageBasis - Start");
    #endif

    unsigned int w = rs->width;
    unsigned int h = rs->height;
    unsigned char red, green, blue;
//    ESP_LOGD(TAG, "Image: %s size: %d x %d\n", _fn.c_str(), w, h);

    input_i = 0;
    float* input_data_ptr = (interpreter->input(0))->data.f;

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            {
                red = rs->GetPixelColor(x, y, 0);
                green = rs->GetPixelColor(x, y, 1);
                blue = rs->GetPixelColor(x, y, 2);
                *(input_data_ptr) = (float) red;
                input_data_ptr++;
                *(input_data_ptr) = (float) green;
                input_data_ptr++;
                *(input_data_ptr) = (float) blue;
                input_data_ptr++;
            }

    #ifdef DEBUG_DETAIL_ON 
        LogFile.WriteHeapInfo("CTfLiteClass::LoadInputImageBasis - done");
    #endif

    return true;
}



bool CTfLiteClass::MakeAllocate()
{
    MakeStaticResolver();

    #ifdef DEBUG_DETAIL_ON 
        LogFile.WriteHeapInfo("CTLiteClass::Alloc start");
    #endif

    LogFile.WriteToFile(ESP_LOG_DEBUG, TAG, "CTfLiteClass::MakeAllocate");
    this->interpreter = new tflite::MicroInterpreter(this->model, resolver, this->tensor_arena, this->kTensorArenaSize);
    LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Trying to load the model. If it crashes here, it ist most likely due to a corrupted model!");

    if (this->interpreter) 
    {
        TfLiteStatus allocate_status = this->interpreter->AllocateTensors();
        if (allocate_status != kTfLiteOk) {
            LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "AllocateTensors() failed");

            this->GetInputDimension();   
            return false;
        }
    }
    else 
    {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "new tflite::MicroInterpreter failed");
        LogFile.WriteHeapInfo("CTfLiteClass::MakeAllocate-new tflite::MicroInterpreter failed");
        return false;
    }


    #ifdef DEBUG_DETAIL_ON 
        LogFile.WriteHeapInfo("CTLiteClass::Alloc done");
    #endif

    return true;
}


void CTfLiteClass::GetInputTensorSize()
{
#ifdef DEBUG_DETAIL_ON    
    float *zw = this->input;
    int test = sizeof(zw);
    ESP_LOGD(TAG, "Input Tensor Dimension: %d", test);
#endif
}


long CTfLiteClass::GetFileSize(std::string filename)
{
  struct stat stat_buf;
  long rc = -1;

  FILE *pFile = fopen(filename.c_str(), "rb"); // previously only "rb

  if (pFile != NULL)
  {
    rc = stat(filename.c_str(), &stat_buf);
    fclose(pFile);
  }
  
  return rc == 0 ? stat_buf.st_size : -1;
}


bool CTfLiteClass::ReadFileToModel(std::string _fn)
{
    LogFile.WriteToFile(ESP_LOG_DEBUG, TAG, "CTfLiteClass::ReadFileToModel: " + _fn);

    const bool is_gz = ends_with(_fn, ".gz");
    long file_size = GetFileSize(_fn);
    if (file_size == -1)
    {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Model file doesn't exist: " + _fn + "!");
        return false;
    }

    uint32_t expected_size_u32 = 0;
    long expected_size = file_size;
    if (is_gz) {
        FILE *pf = fopen(_fn.c_str(), "rb");
        if (!pf) {
            LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Unable to open model: " + _fn);
            return false;
        }
        if (!gzip_get_isize(pf, file_size, expected_size_u32)) {
            fclose(pf);
            LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Unable to read gzip footer (ISIZE) for model: " + _fn);
            return false;
        }
        fclose(pf);
        expected_size = (long)expected_size_u32;
    }

    if (expected_size <= 0 || expected_size > (long)MAX_MODEL_SIZE) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Unable to load model '" + _fn + "'! Decompressed size does not fit in reserved PSRAM (size=" +
                                               std::to_string(expected_size) + ", max=" + std::to_string((long)MAX_MODEL_SIZE) + ")");
        return false;
    }

    LogFile.WriteToFile(ESP_LOG_DEBUG, TAG, "Loading Model " + _fn + " /size: " + std::to_string(expected_size) + " bytes...");

#ifdef DEBUG_DETAIL_ON      
        LogFile.WriteHeapInfo("CTLiteClass::Alloc modelfile start");
#endif

    modelfile = (unsigned char*)psram_get_shared_model_memory();
  
    if (modelfile != NULL)
    {
        FILE *pFile = fopen(_fn.c_str(), "rb"); // previously only "rb
    
        if (pFile != NULL)
        {
          if (!is_gz) {
              fread(modelfile, 1, expected_size, pFile);
              fclose(pFile);
          } else {
              const long full_size = file_size;
              long deflate_offset = 0;
              if (!gzip_skip_header(pFile, full_size, deflate_offset)) {
                  fclose(pFile);
                  LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Gzip header parse failed for model: " + _fn);
                  return false;
              }

              const long deflate_end = full_size - 8; // exclude gzip footer
              if (deflate_end <= deflate_offset) {
                  fclose(pFile);
                  LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Gzip data section invalid for model: " + _fn);
                  return false;
              }

              if (fseek(pFile, deflate_offset, SEEK_SET) != 0) {
                  fclose(pFile);
                  LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Failed to seek gzip deflate stream for model: " + _fn);
                  return false;
              }

              tinfl_decompressor decomp;
              tinfl_init(&decomp);

              uint8_t inbuf[1024];
              size_t in_avail = 0;
              size_t out_pos = 0;
              long in_off = deflate_offset;

              while (true) {
                  if (in_avail == 0) {
                      const long remaining = deflate_end - in_off;
                      if (remaining <= 0) {
                          fclose(pFile);
                          LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Gzip stream ended unexpectedly for model: " + _fn);
                          return false;
                      }
                      const size_t to_read = (size_t)std::min<long>(remaining, (long)sizeof(inbuf));
                      const size_t r = fread(inbuf, 1, to_read, pFile);
                      if (r == 0) {
                          fclose(pFile);
                          LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Failed to read gzip stream for model: " + _fn);
                          return false;
                      }
                      in_avail = r;
                      in_off += (long)r;
                  }

                  const uint8_t *in_ptr = inbuf;
                  size_t in_bytes = in_avail;
                  size_t out_bytes = (size_t)expected_size - out_pos;

                  mz_uint32 flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
                  if (in_off < deflate_end) {
                      flags |= TINFL_FLAG_HAS_MORE_INPUT;
                  }

                  tinfl_status st = tinfl_decompress(&decomp,
                                                     in_ptr, &in_bytes,
                                                     (mz_uint8 *)modelfile,
                                                     (mz_uint8 *)modelfile + out_pos, &out_bytes,
                                                     flags);

                  // in_bytes/out_bytes are the amount consumed/produced
                  in_avail -= in_bytes;
                  if (in_avail > 0) {
                      memmove(inbuf, inbuf + in_bytes, in_avail);
                  }
                  out_pos += out_bytes;

                  if (st == TINFL_STATUS_DONE) {
                      break;
                  }
                  if (st < TINFL_STATUS_DONE) {
                      fclose(pFile);
                      LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Gzip inflate failed for model: " + _fn + " (status=" + std::to_string((int)st) + ")");
                      return false;
                  }
                  if (st == TINFL_STATUS_HAS_MORE_OUTPUT) {
                      fclose(pFile);
                      LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Gzip model too large for buffer: " + _fn);
                      return false;
                  }
              }

              fclose(pFile);

              if ((long)out_pos != expected_size) {
                  LogFile.WriteToFile(ESP_LOG_WARN, TAG, "Gzip model decompressed size mismatch: expected " +
                                                     std::to_string(expected_size) + " got " + std::to_string(out_pos));
              }
          }

#ifdef DEBUG_DETAIL_ON
          LogFile.WriteHeapInfo("CTLiteClass::Alloc modelfile successful");
#endif

          return true;
        }
        else
        {
          LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "CTfLiteClass::ReadFileToModel: Model does not exist");
          return false;
        }
    }   
    else 
    {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "CTfLiteClass::ReadFileToModel: Can't allocate enough memory: " + std::to_string(expected_size));
        LogFile.WriteHeapInfo("CTfLiteClass::ReadFileToModel");

        return false;
    }
}


bool CTfLiteClass::LoadModel(std::string _fn)
{
    LogFile.WriteToFile(ESP_LOG_DEBUG, TAG, "CTfLiteClass::LoadModel");

    if (!ReadFileToModel(_fn.c_str())) {
      return false;
    }

    model = tflite::GetModel(modelfile);

    if(model == nullptr)     
      return false;
    
    return true;
}


CTfLiteClass::CTfLiteClass()
{
    this->model = nullptr;
    this->modelfile = NULL;
    this->interpreter = nullptr;
    this->input = nullptr;
    this->output = nullptr;
    this->kTensorArenaSize = TENSOR_ARENA_SIZE;
    this->tensor_arena = (uint8_t*)psram_get_shared_tensor_arena_memory();
}


CTfLiteClass::~CTfLiteClass()
{
  delete this->interpreter;

  psram_free_shared_tensor_arena_and_model_memory();
}        
