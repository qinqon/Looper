#include <ADC.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>

#include "ButtonStrip.h"
#include "LooperInterface.h"
#include "SDAudioRecorder.h"

constexpr int SDCARD_CS_PIN    = BUILTIN_SDCARD;
constexpr int SDCARD_MOSI_PIN  = 11;
constexpr int SDCARD_SCK_PIN   = 13;

constexpr int I2C_ADDRESS(0x01); 
constexpr int STOP_LOOP_BUTTON_DOWN_TIME_MS(2000);

constexpr int MAX_SAMPLES(12);
char* sample_files[MAX_SAMPLES];
int num_samples_loaded        = 0;

// wrap in a struct to ensure initialisation order
struct IO
{
  ADC                         adc;
  AudioInputAnalog            audio_input;
  AudioOutputAnalog           audio_output;

  IO() :
    adc(),
    audio_input(A0),
    audio_output()
  {
  }
};

IO io;

SD_AUDIO_RECORDER audio_recorder;

AudioAmplifier    input_gain;
AudioMixer4       looper_mixer;
AudioEffectDelay  delay_line;
AudioMixer4       delay_feedback_mixer;
AudioMixer4       output_mixer;

AudioConnection   patch_cord_1( io.audio_input, 0, input_gain, 0 );
AudioConnection   patch_cord_2( input_gain, 0, audio_recorder, 0 );
AudioConnection   patch_cord_3( input_gain, 0, looper_mixer, 0 );
AudioConnection   patch_cord_4( audio_recorder, 0, looper_mixer, 1 );
AudioConnection   patch_cord_5( looper_mixer, 0, delay_feedback_mixer, 0 );
AudioConnection   patch_cord_6( looper_mixer, 0, output_mixer, 0 );
AudioConnection   patch_cord_7( output_mixer, 0, io.audio_output, 0 );

// delay section
AudioConnection   patch_cord_8( delay_feedback_mixer, 0, delay_line, 0 );
AudioConnection   patch_cord_9( delay_line, 0, delay_feedback_mixer, 1 );
AudioConnection   patch_cord_10( delay_line, 0, output_mixer, 1 );



BUTTON_STRIP      button_strip( I2C_ADDRESS );

LOOPER_INTERFACE  looper_interface;

//////////////////////////////////////

void set_adc1_to_3v3()
{
  ADC1_SC3 = 0;                 // cancel calibration
  ADC1_SC2 = ADC_SC2_REFSEL(0); // vcc/ext ref 3.3v

  ADC1_SC3 = ADC_SC3_CAL;       // begin calibration

  uint16_t sum;

  while( (ADC1_SC3 & ADC_SC3_CAL))
  {
    // wait
  }

  __disable_irq();

    sum = ADC1_CLPS + ADC1_CLP4 + ADC1_CLP3 + ADC1_CLP2 + ADC1_CLP1 + ADC1_CLP0;
    sum = (sum / 2) | 0x8000;
    ADC1_PG = sum;
    sum = ADC1_CLMS + ADC1_CLM4 + ADC1_CLM3 + ADC1_CLM2 + ADC1_CLM1 + ADC1_CLM0;
    sum = (sum / 2) | 0x8000;
    ADC1_MG = sum;

  __enable_irq();
  
}

// find all .raw files in dir and add to sample list
void fill_sample_list( File dir )
{
  for( int i = 0; i < MAX_SAMPLES; ++i )
  {
    sample_files[i] = nullptr;  
  }
  
  while(1)
  {
    File entry = dir.openNextFile();
    if( !entry )
    {
      // done!
      return;
    }

    if( !entry.isDirectory() )
    {
      const int entry_filename_length = strlen(entry.name());
      constexpr const char* file_ext = ".RAW";
      constexpr int file_ext_length = strlen( file_ext );
      const char* entry_ext = entry.name() + entry_filename_length - file_ext_length;
      
      if( entry_filename_length > file_ext_length && strncmp( entry_ext, file_ext, file_ext_length ) == 0 )
      { 
        sample_files[num_samples_loaded] = new char[entry_filename_length+1];
        strcpy( sample_files[num_samples_loaded], entry.name() );
  
        if( ++num_samples_loaded == MAX_SAMPLES )
        {
          // sample list full
          return;
        }
      }
    }
  }
}

void setup()
{
#ifdef DEBUG_OUTPUT
  Serial.begin(9600);
  serial_port_initialised = true;
#endif

  constexpr int mem_size = 512;
  AudioMemory( mem_size );

  analogReference(INTERNAL);

  set_adc1_to_3v3();

  // Initialize the SD card
  SPI.setMOSI(SDCARD_MOSI_PIN);
  SPI.setSCK(SDCARD_SCK_PIN);
  
  if( !( SD.begin(SDCARD_CS_PIN) ) )
  {
    // stop here if no SD card, but print a message
    while (1)
    {
      DEBUG_TEXT_LINE("Unable to access the SD card");
      delay(500);
    }
  }

  File root = SD.open("/");
  fill_sample_list( root );

  DEBUG_TEXT_LINE("Files:");
  for( int i = 0; i < num_samples_loaded; ++i )
  {
    DEBUG_TEXT_LINE( sample_files[i] );
  }

  looper_interface.setup( num_samples_loaded );

  Wire.begin( I2C_ADDRESS );

  DEBUG_TEXT_LINE("Setup finished!\n");
  delay(500);
}

void update_looper_mode(uint64_t time_ms)
{
  static bool in_record_mode = looper_interface.mode() == LOOPER_INTERFACE::MODE::LOOP_RECORD;
  static bool mode_change_pending = false;

  switch( looper_interface.mode() )
  {
    case LOOPER_INTERFACE::MODE::LOOP_PLAY:
    {
      if( in_record_mode )
      {
        // switching from loop record to loop playback
        audio_recorder.play();

        looper_interface.set_recording( false, time_ms ); // may have been recording overdub
        looper_interface.set_mode_pending( true, time_ms );

        mode_change_pending = true;
      }
      in_record_mode = false;

      // apply once mode has changed
      if( mode_change_pending && !audio_recorder.mode_pending() )
      {
        mode_change_pending = false;

        looper_interface.set_mode_pending( false, time_ms );
        button_strip.start_free_play_sequence( audio_recorder.play_back_file_time_ms(), time_ms );
        button_strip.lock_buttons( false );
      }

      if( looper_interface.record_button().single_click() )
      {
        // sequence recording
        switch( button_strip.mode() )
        {
          case BUTTON_STRIP::MODE::FREE_PLAY:
          {
            // start recording
            button_strip.start_record_sequence( time_ms );
            looper_interface.set_recording( true, time_ms );
            break;
          }
          case BUTTON_STRIP::MODE::RECORD_SEQ:
          {
            // stop recording
            button_strip.start_sequence_playback( time_ms );
            looper_interface.set_recording( false, time_ms );
            break;
          }
          case BUTTON_STRIP::MODE::PLAY_SEQ:
          {
            // nothing to do here?
            break;
          }
        }
      }
      else if( looper_interface.record_button().down_time_ms() > STOP_LOOP_BUTTON_DOWN_TIME_MS &&
                button_strip.mode() != BUTTON_STRIP::MODE::FREE_PLAY )
      {
        looper_interface.set_recording( false, time_ms );

        audio_recorder.play();

        button_strip.start_free_play_sequence( audio_recorder.play_back_file_time_ms(), time_ms );
      }

      break;
    }
    case LOOPER_INTERFACE::MODE::LOOP_RECORD:
    {
      button_strip.lock_buttons( true );
      
      if( !in_record_mode )
      {
        // switching from loop playback to loop record
        if( button_strip.mode() == BUTTON_STRIP::MODE::RECORD_SEQ || button_strip.mode() == BUTTON_STRIP::MODE::PLAY_SEQ )
        {
          button_strip.clear_sequence();
        }
        
        audio_recorder.start_record();

        looper_interface.set_recording( false, time_ms ); // may have been recording pattern sequence
        looper_interface.set_mode_pending( true, time_ms );

        mode_change_pending = true;
      }
      in_record_mode = true;

      // apply once mode has changed
      if( mode_change_pending && !audio_recorder.mode_pending() )
      {
        mode_change_pending = false;

        looper_interface.set_mode_pending( false, time_ms );
        button_strip.start_free_play_sequence( audio_recorder.play_back_file_time_ms(), time_ms );
      }
      
      if( looper_interface.record_button().single_click() )
      {
        // TODO consider not exposing mode - only controls        
        switch( audio_recorder.mode() )
        {
          case SD_AUDIO_RECORDER::MODE::STOP:
          {
            // start recording over the top
            audio_recorder.start_record();
            looper_interface.set_recording( true, time_ms );

            break;
          }
          case SD_AUDIO_RECORDER::MODE::RECORD_INITIAL:
          {
            // stop recording and play loop
            audio_recorder.stop_record();
            looper_interface.set_recording( false, time_ms );
            button_strip.start_free_play_sequence( audio_recorder.play_back_file_time_ms(), time_ms );
            
            break;
          }
          case SD_AUDIO_RECORDER::MODE::RECORD_PLAY:
          {
            looper_interface.set_recording( true, time_ms );
            
            audio_recorder.start_record(); // start overdubbing
            
            break;
          }
          case SD_AUDIO_RECORDER::MODE::RECORD_OVERDUB:
          {
            looper_interface.set_recording( false, time_ms );
            
            audio_recorder.stop_record(); // stop overdubbing
            
            break;
          }
          case SD_AUDIO_RECORDER::MODE::PLAY:
          { 
            DEBUG_TEXT_LINE("Record during play not supported"); // eventually this should record a sequence of key presses
            break;           
          }
          default:
          {
            // further modes to come..
            DEBUG_TEXT_LINE("Unknown looper mode");
            break;
          }
        }
      }
      else if( looper_interface.record_button().down_time_ms() > STOP_LOOP_BUTTON_DOWN_TIME_MS )
      {
        audio_recorder.stop();

        looper_interface.set_recording( false, time_ms );

        button_strip.stop_sequence();
      }
      break;
    }
    default:
    {
      DEBUG_TEXT_LINE("Error:what mode is this");
      break;
    }
  }
}

void loop()
{
  const uint64_t time_ms = millis();

  looper_interface.update( io.adc, time_ms );

  update_looper_mode( time_ms );

  audio_recorder.update_main_loop(); 

  // set interface paramaters
  audio_recorder.set_saturation( looper_interface.saturation() );
  audio_recorder.set_speed( looper_interface.play_back_speed() );
  
  input_gain.gain( looper_interface.gain() );

  delay_line.delay( 0, looper_interface.delay_time() );

  const float looper_mix = looper_interface.looper_mix();
  looper_mixer.gain( 0, 1.0f - looper_mix );
  looper_mixer.gain( 1, looper_mix );

  const float delay_feedback = looper_interface.delay_feedback();
  delay_feedback_mixer.gain( 0, 1.0f );
  delay_feedback_mixer.gain( 1, delay_feedback );

  const float delay_mix = looper_interface.delay_mix();
  output_mixer.gain( 0, 1.0f - delay_mix );
  output_mixer.gain( 1, delay_mix );

  uint32_t activated_segment;
  const float playback_pos = audio_recorder.playback_position();
  uint32_t overridden_segment = clamp<uint32_t>( playback_pos * BUTTON_STRIP::NUM_SEGMENTS, 0, BUTTON_STRIP::NUM_SEGMENTS - 1 );
  if( button_strip.update( time_ms, activated_segment, overridden_segment ) )
  {
    if( audio_recorder.mode() == SD_AUDIO_RECORDER::MODE::PLAY )
    {
      const float t = activated_segment / static_cast<float>(button_strip.num_segments());
      audio_recorder.set_read_position( t );
    }
  }

/*
  const int processor_usage = AudioProcessorUsage();
  if( processor_usage > 45 )
  {
    Serial.print( "Performance spike: " );
    Serial.print( processor_usage );
    Serial.print( "\n" );
  }
*/
}
