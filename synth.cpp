/* multiplex modules: must take at least one vectors of buffers in, maintains
   multiple channels for output, but can mixdown if a mono module requests.

   what has to be multiplexed?

   maybe we really should treat stereo as a special case of multiplexing :/
   if so, creating and destroying channels might not be reasonable

   should be a way to create and destroy channels.  presumably create on
   note-on, how do we know when to destroy?  some way to flag, like when the
   envgen goes idle?  note-off and n samples of silence?  should be after the
   reverb trails off.  can't check for that, though, because we're not
   reverbing each channel individually.

   Todo:
   - support relative bending after touchdown
   - sample playback (wave reader)
   - text file patch definitions
   - sequencer (retriggerable)
   - multiple intonations!  just, meantone, quarter tone, well-tempered, etc.
   - replay output from wave, to find nasty clicks (wave reader)
   - hard clipper
   - rectifier
   - oscillator hardsync
   - oscillator band-limiting (http://www.fly.net/~ant/bl-synth/ ?)
   - slew limiter
   - switch?
   - additional filters?  eq?
   - reverb
   - exponential envgen, DADSR, parameterized shape
   - multiplexing subsystem
   Done:
   - x/y input
   - scale quantizer -- actually, "scale" should be a parameter of
     "notetofrequency"!
   - stereo
   - pan module
   - stereoadd module
   - ping pong delay
   - stereo rotate
   - hard/soft-limiter (overdrive)
   - panner
   - output range calculation
   - rename unitscaler to rescaler; make it scale input range to new range
   - input range validation
*/
#include "synth.h"

#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "exception.h"
#include "input.h"

using namespace std;

#ifndef __APPLE__
#define snprintf _snprintf
#endif //__APPLE__

#define _CRT_SECURE_NO_WARNINGS
#pragma warning(disable:4244) // double conversion to float
#pragma warning(disable:4305) // double conversion to float
#pragma warning(disable:4996) // fopen() purportedly unsafe
#pragma warning(disable:4267) // size_t conversion to int

const float pi = 3.1415926535897932384626f, e = 2.71828183f;
const float note_0 = 8.1757989156;
const int max_buffer_size = 4000;
const int sample_rate = 44100;

struct
{ const char *name;
  const char *steps;
} scales[] =
{
  { "major",      "\2\2\1\2\2\2\1" },
  { "minor",      "\2\1\2\2\1\2\2" },
  { "dorian",     "\2\1\2\2\2\1\2" },
  { "phrygian",   "\1\2\2\2\1\2\2" },
  { "lydian",     "\2\2\2\1\2\2\1" },
  { "mixolydian", "\2\2\1\2\2\1\2" },
  { "locrian",    "\1\2\2\1\2\2\2" },
  { "pentatonic", "\2\2\3\2\3" },
  { "pent minor", "\3\2\2\3\2" },
  { "chromatic",  "\1" },
  { "whole",      "\2" },
  { "minor 3rd",  "\3" },
  { "3rd",        "\4" },
  { "4ths",       "\5" },
  { "tritone",    "\6" },
  { "5ths",       "\7" },
  { "octave",     "\12" },
  { 0,            0 }
};

EXCEPTION_D(CouldntWriteExcept, Exception, "Couldn't open for writing")

class WaveOut
{
  public:
    WaveOut(const std::string filename, float scaler=32768, bool stereo=false)
    : m_scaler(scaler), m_length(0)
    {
      m_out = fopen(filename.c_str(), "wb");
      if(!m_out) throw CouldntWriteExcept("couldn't open file for writing");
      unsigned char header[] =
      {
        'R', 'I', 'F', 'F',
        0x00, 0x00, 0x00, 0x00, // wave size+36; patch later
        'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
        0x10, 0x00, 0x00, 0x00, 0x01, 0x00, // PCM
        0x01, 0x00, // channels
        0x44, 0xAC, 0x00, 0x00, // 44.1khz
        0x10, 0xB1, 0x02, 0x00, // 176400 bytes/sec
        0x02, 0x00, // bytes per sample*channels
        0x10, 0x00, // 16 bits
        'd', 'a', 't', 'a',
        0x00, 0x00, 0x00, 0x00, // wave size again
      };
      if(stereo) header[22] = 0x02, header[32] = 0x04;
      fwrite(header, sizeof(header), 1, m_out);
    }

    ~WaveOut() { close(); }

    void close()
    {
      if(!m_out) return;
      updateLength();
      fclose(m_out);
      m_out = 0;
    }

    void writeBuffer(const float *buffer, int size)
    {
      static short out[max_buffer_size];
      for(int i=0; i<size; i++) out[i] = short(buffer[i]*m_scaler);
      fwrite(out, 2, size, m_out);
      m_length += size;

      updateLength();
    }

  private:
    void updateLength()
    {
      int chunklength = m_length*2;
      fseek(m_out, 40, SEEK_SET);
      fwrite(&chunklength, 4, 1, m_out);

      chunklength += 36;
      fseek(m_out, 4, SEEK_SET);
      fwrite(&chunklength, 4, 1, m_out);


      fseek(m_out, 0, SEEK_END);
    }

    FILE *m_out;
    float m_scaler;
    int m_length;
};

EXCEPTION_D(InvalidInputRangeExcept, Exception, "Invalid Input Range")
EXCEPTION_D(InvalidOutputRangeExcept, Exception, "Invalid Output Range")

class Module
{
  public:
    Module() : m_last_fill(0), m_waveout(0)
    {
      memset(m_output, 0, max_buffer_size*sizeof(float));
      //memset(&m_within_lower_bound, 0xff, sizeof(float));
      //memset(&m_within_upper_bound, 0xff, sizeof(float));
    }
    ~Module()
    {
      if(m_waveout) delete m_waveout;
    }

    virtual void fill(float last_fill, int samples) = 0;
    virtual void getOutputRange(float *out_min, float *out_max) = 0;
    virtual const char *moduleName() = 0;
    virtual void validateInputRange() = 0;
    virtual bool stereo() const { return false; }

    virtual const float *output(float last_fill, int samples)
    {
      if(m_last_fill < last_fill)
      {
        validateInputRange();
        fill(last_fill, samples);
        m_last_fill = last_fill;
        if(m_waveout) m_waveout->writeBuffer(m_output, samples);
        #ifndef NDEBUG
        validateOutputRange(m_output, samples);
        #endif
      }

      return m_output;
    }

    virtual void log(const string filename, float scaler=32768)
    {
      if(!m_waveout) m_waveout = new WaveOut(filename, scaler);
    }

    void validateWithin(Module &input, float min, float max)
    {
      float inp_min, inp_max;
      input.getOutputRange(&inp_min, &inp_max);
      if(inp_min < min || inp_max > max)
      {
        static char error[256];
        snprintf(error, 255,
                  "%s (%g, %g) for %s (%g, %g)",
                  input.moduleName(), inp_min, inp_max, moduleName(), min, max);
        throw InvalidInputRangeExcept(error);
      }
    }

    void validateOutputRange(float *buffer, int samples)
    {
      float min, max;
      getOutputRange(&min, &max);
      for(int i=0; i<samples; i++)
      {
        if(buffer[i] < min || m_output[i] > max)
        {
          static char error[256];
          getOutputRange(&min, &max);
          snprintf(error, 255, "%s: %g <= %g <= %g",
                   moduleName(), min, m_output[i], max);
          throw InvalidOutputRangeExcept(error);
        }
/*        float upper_bound_distance = abs(max-m_output[i]);
        float lower_bound_distance = abs(min-m_output[i]);
        if(_isnan(m_within_upper_bound) || upper_bound_distance<m_within_upper_bound) m_within_upper_bound = upper_bound_distance;
        if(_isnan(m_within_lower_bound) || lower_bound_distance<m_within_lower_bound) m_within_lower_bound = lower_bound_distance; */
      }
    }

  protected:
    float m_output[max_buffer_size];
    float m_last_fill;
    //float m_within_lower_bound;
    //float m_within_upper_bound;
    WaveOut *m_waveout;
};

class StereoModule : public Module
{
  public:
    StereoModule() : Module()
    {
      memset(m_output, 0, max_buffer_size*2*sizeof(float));
    }

    bool stereo() const { return true; }

    const float *output(float last_fill, int samples)
    {
      if(m_last_fill < last_fill)
      {
        fill(last_fill, samples);
        m_last_fill = last_fill;
        if(m_waveout) m_waveout->writeBuffer(m_output, samples*2);
        validateOutputRange(m_output, samples*2);
      }

      return m_output;
    }

    void log(const string filename, float scaler=32768)
    {
      if(!m_waveout) m_waveout = new WaveOut(filename, scaler, true);
    }

  protected:
    float m_output[max_buffer_size*2];
};

union ModuleParam
{
  Module       *m_module;
  StereoModule *m_stereomodule;
  float         m_float;
  int           m_int;
  char         *m_string;
};

class Constant : public Module
{
  public:
    Constant(float value) { setValue(value); }
  
    const char *moduleName() { return "Constant"; }

    void fill(float last_fill, int samples) {}

    void setValue(float value)
    {
      for(int i=0; i<max_buffer_size; i++) m_output[i] = value;
    }
 
    void validateInputRange() {} // no input

    void getOutputRange(float *out_min, float *out_max)
    {      
      *out_min = *out_max = m_output[0];
    }
};

class Input : public Module
{
  public:
    Input(int axis) : m_axis(axis) {}
    
    static Module *create(vector<ModuleParam *> parameters)
    {
      return new Input(parameters[0]->m_int);
    }
    
    const char *moduleName() { return "Input"; }

    void fill(float last_fill, int samples)
    {
      readInputAxis(m_axis, m_output, samples);
    }
  
    void getOutputRange(float *out_min, float *out_max)
    {      
      *out_min = (m_axis==2)?0:-1;
      *out_max = 1;
    }

    void validateInputRange() {} // no input

  private:
    int m_axis;
};

EXCEPTION(ParseExcept, Exception, "Parse Error")
EXCEPTION(TooManyParamsExcept, ParseExcept, "Too many parameters")

class ModuleInfo
{
  public:
    ModuleInfo(string name, Module *(*instantiator)(vector<ModuleParam *>))
    : m_name(name), m_instantiator(instantiator) {}

    void addParameter(string name, string type)
    {
      m_parameters.push_back(pair<string, string>(name, type));
    }
    
    const pair<string, string> parameter(unsigned int n) const
    {
      if(n>=m_parameters.size())
        throw TooManyParamsExcept();
      return m_parameters[n];
    }
    
    int parameterCount() const { return m_parameters.size(); }
    
    Module *instantiate(vector<ModuleParam *> parameters)
    {
      return m_instantiator(parameters);
    }
    
  private:
    string m_name;
    vector<pair<string, string> > m_parameters; // name, type
    Module *(*m_instantiator)(vector<ModuleParam *> parameters);
};

map<string, ModuleInfo *> g_module_infos;

#include "modules_generated.cpp"

map<string, Module *> g_modules;

EXCEPTION_D(UnknownModuleTypeExcept, ParseExcept, "Unknown module type")
EXCEPTION_D(ExpectingFloatExcept, ParseExcept, "Expecting a float")
EXCEPTION_D(ExpectingIntExcept, ParseExcept, "Expecting an int")
EXCEPTION_D(ExpectingModuleExcept, ParseExcept, "Expecting module name")
EXCEPTION_D(UnknownModuleExcept, ParseExcept, "Unknown module instance")
EXCEPTION_D(UnknownTypeExcept, ParseExcept, "Unknown type")
EXCEPTION_D(NotStereoExcept, ParseExcept, "Module not stereo")
EXCEPTION_D(NotMonoExcept, ParseExcept, "Module not mono")
EXCEPTION_D(TooFewParamsExcept, ParseExcept, "Too few parameters")

Module *addModule(char *definition)
{
  string def_copy = definition;

  const char *delim = ",() \r\n\t";
  char *t = strtok(definition, delim);
  string name = "", type = "";
  vector<ModuleParam *> params;
  do
  {
    if(type == "")
    {
      type = t;
      if(g_module_infos.count(type) == 0)
        throw UnknownModuleTypeExcept(def_copy+t);
      continue;
    }
    if(name == "") { name = t; continue; }
    string param_type = g_module_infos[type]->parameter(params.size()).second;
    ModuleParam *m = new ModuleParam();
    if(param_type=="float")
    {
      if(!isdigit(*t)) throw ExpectingFloatExcept(def_copy+t);
      m->m_float = atof(t);
    }
    else if(param_type=="int")
    {
      if(!isdigit(*t)) throw ExpectingIntExcept(def_copy+t);
      m->m_int = atof(t);
    }
    else if(param_type=="Module")
    {
      if(isdigit(*t))
        m->m_module = new Constant(atof(t));
      else
      {
        if(!g_modules.count(t)) throw UnknownModuleExcept(def_copy+t);
        if(g_modules[t]->stereo())
          throw NotMonoExcept(def_copy + t);
        m->m_module = g_modules[t];
      } 
    }
    else if(param_type=="StereoModule")
    {
      if(!isalpha(*t))
        throw ExpectingModuleExcept(def_copy+t);
      else
      {
        if(!g_modules.count(t)) throw UnknownModuleExcept(def_copy+t);
        if(!g_modules[t]->stereo())
          throw NotStereoExcept(def_copy + t);
        m->m_module = g_modules[t];
      } 
    }
    /*else if(param_type=="string")
    {
      throw "unimplemented";
    }*/
    else throw UnknownTypeExcept(def_copy+type);
    params.push_back(m);
  } while(t = strtok(0, delim));
  if(params.size() != g_module_infos[type]->parameterCount())
    throw TooFewParamsExcept(def_copy+t);
  g_modules[name] = g_module_infos[type]->instantiate(params);
  
  return g_modules[name];
}

EXCEPTION(NoOutputModuleExcept, Exception, "No output module")
EXCEPTION(OutputNotStereoExcept, Exception, "Output not stereo")

void produceStream(short *buffer, int samples)
{ 
  fillModuleList();
  g_module_infos["Input"] = new ModuleInfo("Input", Input::create);
  g_module_infos["Input"]->addParameter("axis", "int");

  static bool first = true;
  static Module *output;
  if(first)
  {
    first = false;
    FILE *in = fopen("patch.txt", "r");
    char s[256];
    while(!feof(in))
    {
      fgets(s, 255, in);
      addModule(s);
    }
    if(!g_modules.count("output")) throw NoOutputModuleExcept();
    output = g_modules["output"];
    if(!output->stereo()) throw OutputNotStereoExcept();
  }
  
  static float time = 0;
  const float *o = output->output(time, samples);
  time += 1.0;
  
  for(int i=0; i<samples*2; i+=2)
  {
#ifdef __APPLE__
    *buffer++ = short(o[i]+o[i+1] * 16384);
#else
    *buffer++ = short(o[i]   * 32767);
    *buffer++ = short(o[i+1] * 32767);
#endif
  }
}