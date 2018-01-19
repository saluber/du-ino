/*
 * ####                                                ####
 * ####                                                ####
 * ####                                                ####      ##
 * ####                                                ####    ####
 * ####  ############  ############  ####  ##########  ####  ####
 * ####  ####    ####  ####    ####  ####  ####        ########
 * ####  ####    ####  ####    ####  ####  ####        ########
 * ####  ####    ####  ####    ####  ####  ####        ####  ####
 * ####  ####    ####  ####    ####  ####  ####        ####    ####
 * ####  ############  ############  ####  ##########  ####      ####
 *                             ####                                ####
 * ################################                                  ####
 *            __      __              __              __      __       ####
 *   |  |    |  |    [__)    |_/     (__     |__|    |  |    [__)        ####
 *   |/\|    |__|    |  \    |  \    .__)    |  |    |__|    |             ##
 *
 *
 * DU-INO DU-SEQ Emulator
 * Aaron Mavrinac <aaron@logick.ca>
 */

#include <du-ino_function.h>
#include <du-ino_interface.h>
#include <du-ino_dsp.h>
#include <TimerOne.h>
#include <avr/pgmspace.h>

#define DIGITAL_THRESH 3.0 // V

enum GateMode {
  GATE_NONE = 0,
  GATE_1SHT = 1,
  GATE_REPT = 2,
  GATE_LONG = 3,
  GATE_EXT1 = 4,
  GATE_EXT2 = 5
};

enum Intonation {
  IN,
  IF,
  IS
};

static const unsigned char gate_mode_icons[] PROGMEM = {
  0x00, 0x22, 0x14, 0x08, 0x14, 0x22, 0x00,
  0x1C, 0x22, 0x5D, 0x5D, 0x5D, 0x22, 0x1C,
  0x7C, 0x00, 0x7F, 0x00, 0x7F, 0x00, 0x7C,
  0x08, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x08,
  0x30, 0x40, 0x52, 0x5F, 0x50, 0x40, 0x30,
  0x30, 0x40, 0x59, 0x55, 0x52, 0x40, 0x30
};

static const unsigned char semitone_lt[12] = {'C', 'C', 'D', 'E', 'E', 'F', 'F', 'G', 'G', 'A', 'B', 'B'};
static const Intonation semitone_in[12] = {IN, IS, IN, IF, IN, IN, IS, IN, IS, IN, IF, IN};

struct DU_SEQ_Values {
  float stage_cv[8];
  uint8_t stage_steps[8];
  GateMode stage_gate[8];
  bool stage_slew[8];
  uint8_t stage_count;
  bool diradd_mode;
  float slew_hz;
  uint16_t gate_ms;
  unsigned long clock_period;
  bool clock_ext;
};

volatile DU_SEQ_Values seq_values;

volatile uint8_t stage, step;
volatile bool gate, clock_gate, retrigger, reverse;

DUINO_Filter * slew_filter;

void clock_isr();
void reset_isr();

void update_clock();

class DU_SEQ_Function : public DUINO_Function {
 public:
  DU_SEQ_Function() : DUINO_Function(0b0111111111) { }
  
  virtual void setup()
  {
    // TODO: attach GT3 to hardware clock isr that sets clock to EXT
    gt_attach_interrupt(GT3, clock_isr, CHANGE);
    gt_attach_interrupt(GT4, reset_isr, RISING);
  }

  virtual void loop()
  {
    // cache stage, step, and clock gate (so that each loop is "atomic")
    cached_stage = stage;
    cached_step = step;
    cached_clock_gate = clock_gate;
    cached_retrigger = retrigger;
    retrigger = false;

    if(cached_retrigger)
    {
      if(!cached_step)
      {
        // drop gate and clock at start of stage
        gt_out(GT_MULTI | (1 << GT5) | (1 << GT6), false);
      }
      else
      {
        // drop clock each retrigger
        gt_out(GT6, false);
      }
      clock_time = millis();
    }

    // set gate state
    switch(seq_values.stage_gate[cached_stage])
    {
      case GATE_NONE:
        gate = false;
        break;
      case GATE_1SHT:
        if(!cached_step)
        {
          gate = partial_gate();
        }
        break;
      case GATE_REPT:
        gate = partial_gate();
        break;
      case GATE_LONG:
        if(cached_step == seq_values.stage_steps[cached_stage] - 1)
        {
          gate = partial_gate();
        }
        else
        {
          gate = true;
        }
        break;
      case GATE_EXT1:
        gate = gt_read(GT1);
        break;
      case GATE_EXT2:
        gate = gt_read(GT2);
        break;
    }

    // set pitch CV state
    float slew_cv = slew_filter->filter(seq_values.stage_cv[cached_stage]);
    cv_out(CO1, seq_values.stage_slew[cached_stage] ? slew_cv : seq_values.stage_cv[cached_stage]);

    // set gate and clock states
    gt_out(GT5, gate);
    gt_out(GT6, cached_clock_gate);

    // update reverse setting
    if(!seq_values.diradd_mode)
    {
      reverse = cv_read(CI1) > DIGITAL_THRESH;
    }
  }

  uint8_t address_to_stage()
  {
    int8_t addr_stage = (int8_t)cv_read(CI1) * 1.6;
    if(addr_stage < 0)
    {
      addr_stage = 0;
    }
    else if(stage > 7)
    {
      addr_stage = 7;
    }
    return (uint8_t)addr_stage;
  }

 private:
  bool partial_gate()
  {
    return (seq_values.clock_ext && cached_clock_gate)
           || cached_retrigger
           || ((millis() - clock_time) < seq_values.gate_ms);
  }

  uint8_t cached_stage, cached_step;
  bool cached_clock_gate, cached_retrigger;
  unsigned long clock_time;
};

class DU_SEQ_Interface : public DUINO_Interface {
 public:
  virtual void setup()
  {
    // initialize interface
    main_selected = 2;
    top_selected = 0;
    for(uint8_t i = 0; i < 6; ++i)
    {
      display_changed[i] = false;
    }
    last_gate = false;
    last_stage = 0;
    last_diradd_mode = false;
    last_reverse = false;

    // draw top line
    display->draw_du_logo_sm(0, 0, DUINO_SSD1306::White);
    display->draw_text(16, 0, "SEQ", DUINO_SSD1306::White);

    // draw save box
    display->fill_rect(122, 1, 5, 5, DUINO_SSD1306::White);

    // load settings
    load_params(0, params.bytes, 30);

    // verify settings and export to function parameters
    for(uint8_t i = 0; i < 8; ++i)
    {
      if(params.vals.stage_pitch[i] < 0)
      {
        params.vals.stage_pitch[i] = 0;
      }
      if(params.vals.stage_pitch[i] > 119)
      {
        params.vals.stage_pitch[i] = 119;
      }
      seq_values.stage_cv[i] = note_to_cv(params.vals.stage_pitch[i]);
    }

    for(uint8_t i = 0; i < 8; ++i)
    {
      if(params.vals.stage_steps[i] < 1 || params.vals.stage_steps[i] > 8)
      {
        params.vals.stage_steps[i] = 8;
      }
      seq_values.stage_steps[i] = params.vals.stage_steps[i];
    }
    
    for(uint8_t i = 0; i < 8; ++i)
    {
      if(params.vals.stage_gate[i] < 0 || params.vals.stage_gate[i] > 5)
      {
        params.vals.stage_gate[i] = 2;
      }
      seq_values.stage_gate[i] = (GateMode)params.vals.stage_gate[i];
    }

    for(uint8_t i = 0; i < 8; ++i)
    {
      seq_values.stage_slew[i] = (bool)((params.vals.stage_slew >> i) & 1);
    }

    if(params.vals.stage_count < 1 || params.vals.stage_count > 8)
    {
      params.vals.stage_count = 8;
    }
    seq_values.stage_count = params.vals.stage_count;

    seq_values.diradd_mode = (bool)params.vals.diradd_mode;
    
    if(params.vals.slew_rate < 0 || params.vals.slew_rate > 16)
    {
      params.vals.slew_rate = 8;
    }
    seq_values.slew_hz = slew_hz(params.vals.slew_rate);
    slew_filter->set_frequency(seq_values.slew_hz);

    if(params.vals.clock_bpm < 0 || params.vals.clock_bpm > 99)
    {
      params.vals.clock_bpm = 0;
    }
    seq_values.clock_period = bpm_to_us(params.vals.clock_bpm);
    seq_values.clock_ext = !(bool)params.vals.clock_bpm;
    update_clock();

    if(params.vals.gate_time < 0 || params.vals.gate_time > 16)
    {
      params.vals.gate_time = 8;
    }
    seq_values.gate_ms = params.vals.gate_time * (uint16_t)(seq_values.clock_period / 8000);

    // draw global elements
    for(uint8_t i = 0; i < 6; ++i)
    {
      display->draw_vline(2 + i, 18 - i, i + 1, DUINO_SSD1306::White);
    }
    display->draw_char(10, 12, '0' + params.vals.stage_count, DUINO_SSD1306::White);
    display->draw_char(22, 12, params.vals.diradd_mode ? 'A' : 'D', DUINO_SSD1306::White);
    display->draw_char(30, 12, 0x10, DUINO_SSD1306::White);
    for(uint8_t i = 0; i < 2; ++i)
    {
      display->draw_vline(59 + i * 25, 12, 7, DUINO_SSD1306::White);
      display->draw_hline(60 + i * 25, 12, 16, DUINO_SSD1306::White);
      display->draw_hline(60 + i * 25, 18, 16, DUINO_SSD1306::White);
      display->draw_vline(76 + i * 25, 12, 7, DUINO_SSD1306::White);
    }
    display_slew_rate(60, 13, params.vals.slew_rate, DUINO_SSD1306::White);
    display_gate_time(85, 13, params.vals.gate_time, DUINO_SSD1306::White);
    display_clock(109, 12, params.vals.clock_bpm, DUINO_SSD1306::White);

    // draw step elements
    for(uint8_t i = 0; i < 8; ++i)
    {
      // pitch
      display_note(16 * i, 32, params.vals.stage_pitch[i], DUINO_SSD1306::White);
      // steps
      display->draw_char(16 * i + 1, 49, '0' + params.vals.stage_steps[i], DUINO_SSD1306::White);
      // gate mode
      display->draw_icon_7(16 * i + 8, 49, gate_mode_icons, (GateMode)params.vals.stage_gate[i], DUINO_SSD1306::White);
      // slew
      display->fill_rect(16 * i + 1, 59, 14, 4, DUINO_SSD1306::White);
      display->fill_rect(16 * i + 2 + 6 * (~(params.vals.stage_slew >> i) & 1), 60, 6, 2, DUINO_SSD1306::Black);
    }

    invert_current_selection();
    display->display_all();
  }

  virtual void loop()
  {
    // handle encoder button press
    DUINO_Encoder::Button b = encoder->get_button();
    if(b == DUINO_Encoder::DoubleClicked)
    {
      // switch main selection
      invert_current_selection();
      main_selected++;
      main_selected %= 6;
      invert_current_selection();
    }
    else if(b == DUINO_Encoder::Clicked)
    {
      // switch local selection
      switch(main_selected)
      {
        case 0: // save
          save_params(0, params.bytes, 30);
          display->fill_rect(123, 2, 3, 3, DUINO_SSD1306::Black);
          display_changed[0] = true;
          break;
        case 1: // top
          invert_current_selection();
          top_selected++;
          top_selected %= 5;
          invert_current_selection();
          break;
        default: // stages
          invert_current_selection();
          stage_selected++;
          stage_selected %= 8;
          invert_current_selection();
          break;
      }
    }

    // handle encoder spin
    int16_t v = encoder->get_value();
    if(v)
    {
      switch(main_selected)
      {
        case 0: // save
          break;
        case 1: // top
          switch(top_selected)
          {
            case 0: // count
              params.vals.stage_count += v;
              if(params.vals.stage_count < 1)
              {
                params.vals.stage_count = 1;
              }
              else if(params.vals.stage_count > 8)
              {
                params.vals.stage_count = 8;
              }
              seq_values.stage_count = params.vals.stage_count;
              display->fill_rect(10, 12, 5, 7, DUINO_SSD1306::White);
              display->draw_char(10, 12, '0' + params.vals.stage_count, DUINO_SSD1306::Black);
              break;
            case 1: // dir/add
              if(v < 0)
              {
                params.vals.diradd_mode = 0;
              }
              else if(v > 0)
              {
                params.vals.diradd_mode = 1;
              }
              seq_values.diradd_mode = (bool)params.vals.diradd_mode;
              display->fill_rect(22, 12, 5, 7, DUINO_SSD1306::White);
              display->draw_char(22, 12, params.vals.diradd_mode ? 'A' : 'D', DUINO_SSD1306::Black);
              break;
            case 2: // slew
              params.vals.slew_rate += v;
              if(params.vals.slew_rate < 0)
              {
                params.vals.slew_rate = 0;
              }
              else if(params.vals.slew_rate > 16)
              {
                params.vals.slew_rate = 16;
              }
              seq_values.slew_hz = slew_hz(params.vals.slew_rate);
              slew_filter->set_frequency(seq_values.slew_hz);
              display->fill_rect(60, 13, 16, 5, DUINO_SSD1306::White);
              display_slew_rate(60, 13, params.vals.slew_rate, DUINO_SSD1306::Black);
              break;
            case 3: // gate
              params.vals.gate_time += v;
              if(params.vals.gate_time < 0)
              {
                params.vals.gate_time = 0;
              }
              else if(params.vals.gate_time > 16)
              {
                params.vals.gate_time = 16;
              }
              seq_values.gate_ms = params.vals.gate_time * (uint16_t)(seq_values.clock_period / 8000);
              display->fill_rect(85, 13, 16, 5, DUINO_SSD1306::White);
              display_gate_time(85, 13, params.vals.gate_time, DUINO_SSD1306::Black);
              break;
            case 4: // clock
              params.vals.clock_bpm += v;
              if(params.vals.clock_bpm < 0)
              {
                params.vals.clock_bpm = 0;
              }
              else if(params.vals.clock_bpm > 99)
              {
                params.vals.clock_bpm = 99;
              }
              seq_values.clock_period = bpm_to_us(params.vals.clock_bpm);
              seq_values.clock_ext = !(bool)params.vals.clock_bpm;
              display->fill_rect(109, 12, 17, 7, DUINO_SSD1306::White);
              display_clock(109, 12, params.vals.clock_bpm, DUINO_SSD1306::Black);
              update_clock();
              break;
          }
          display_changed[1] = true;
          break;
        case 2: // pitch
          params.vals.stage_pitch[stage_selected] += v;
          if(params.vals.stage_pitch[stage_selected] < 0)
          {
            params.vals.stage_pitch[stage_selected] = 0;
          }
          else if(params.vals.stage_pitch[stage_selected] > 119)
          {
            params.vals.stage_pitch[stage_selected] = 119;
          }
          seq_values.stage_cv[stage_selected] = note_to_cv(params.vals.stage_pitch[stage_selected]);
          display->fill_rect(16 * stage_selected, 32, 16, 15, DUINO_SSD1306::White);
          display_note(16 * stage_selected, 32, params.vals.stage_pitch[stage_selected], DUINO_SSD1306::Black);
          display_changed[3] = true;
          break;
        case 3: // steps
          params.vals.stage_steps[stage_selected] += v;
          if(params.vals.stage_steps[stage_selected] < 1)
          {
            params.vals.stage_steps[stage_selected] = 1;
          }
          else if(params.vals.stage_steps[stage_selected] > 8)
          {
            params.vals.stage_steps[stage_selected] = 8;
          }
          seq_values.stage_steps[stage_selected] = params.vals.stage_steps[stage_selected];
          display->fill_rect(16 * stage_selected + 1, 49, 5, 7, DUINO_SSD1306::White);
          display->draw_char(16 * stage_selected + 1, 49, '0' + params.vals.stage_steps[stage_selected],
              DUINO_SSD1306::Black);
          display_changed[4] = true;
          break;
        case 4: // gate
          params.vals.stage_gate[stage_selected] += v;
          if(params.vals.stage_gate[stage_selected] < 0)
          {
            params.vals.stage_gate[stage_selected] = 0;
          }
          else if(params.vals.stage_gate[stage_selected] > 5)
          {
            params.vals.stage_gate[stage_selected] = 5;
          }
          seq_values.stage_gate[stage_selected] = (GateMode)params.vals.stage_gate[stage_selected];
          display->fill_rect(16 * stage_selected + 8, 49, 7, 7, DUINO_SSD1306::White);
          display->draw_icon_7(16 * stage_selected + 8, 49, gate_mode_icons,
              (GateMode)params.vals.stage_gate[stage_selected], DUINO_SSD1306::Black);
          display_changed[4] = true;
          break;
        case 5: // slew
          if(v < 0)
          {
            params.vals.stage_slew &= ~(1 << stage_selected);
          }
          else if(v > 0)
          {
            params.vals.stage_slew |= (1 << stage_selected);
          }
          seq_values.stage_slew[stage_selected] = (bool)((params.vals.stage_slew >> stage_selected) & 1);
          display->fill_rect(16 * stage_selected + 1, 59, 14, 4, DUINO_SSD1306::Black);
          display->fill_rect(16 * stage_selected + 2 + 6 * (~(params.vals.stage_slew >> stage_selected) & 1), 60, 6, 2,
              DUINO_SSD1306::White);
          display_changed[5] = true;
          break;
      }

      // mark save box
      if(main_selected)
      {
        if(saved)
        {
          saved = false;
          display->fill_rect(123, 2, 3, 3, DUINO_SSD1306::Black);
          display_changed[0] = true;
        }
      }
    }

    // display reverse/address
    if(seq_values.diradd_mode != last_diradd_mode
        || (!seq_values.diradd_mode && reverse != last_reverse)
        || (seq_values.diradd_mode && stage != last_stage))
    {
      display_reverse_address(30, 12);
      last_diradd_mode = seq_values.diradd_mode;
      last_reverse = reverse;
      display_changed[1] = true;
    }

    // display gate
    if(gate != last_gate || stage != last_stage)
    {
      if(gate)
      {
        if(stage != last_stage)
        {
          display_gate(last_stage, DUINO_SSD1306::Black);
        }
        display_gate(stage, DUINO_SSD1306::White);
      }
      else
      {
        display_gate(last_stage, DUINO_SSD1306::Black);
      }

      last_gate = gate;
      last_stage = stage;
      display_changed[2] = true;
    }

    // update display
    if(display_changed[0])
    {
      display->display(121, 127, 0, 0);
    }
    if(display_changed[1])
    {
      display->display(9, 126, 1, 2);
    }
    if(display_changed[2])
    {
      display->display(6, 121, 3, 3);
    }
    if(display_changed[3])
    {
      display->display(0, 127, 4, 5);
    }
    if(display_changed[5])
    {
      display->display(0, 127, display_changed[4] ? 6 : 7, 7);
    }
  }

 private:
  float note_to_cv(int8_t note)
  {
    return ((float)note - 36.0) / 12.0;
  }

  unsigned long bpm_to_us(uint8_t bpm)
  {
    return 3000000 / (unsigned long)bpm;
  }

  float slew_hz(uint8_t slew_rate)
  {
    if(slew_rate)
    {
      return (float)(17 - slew_rate) / 4.0;
    }
    else
    {
      return 65536.0;
    }
  }

  void display_slew_rate(int16_t x, int16_t y, uint8_t rate, DUINO_SSD1306::SSD1306Color color)
  {
    display->draw_vline(x + rate - 1, y, 5, color);
  }

  void display_gate_time(int16_t x, int16_t y, uint8_t time, DUINO_SSD1306::SSD1306Color color)
  {
    if(time > 1)
    {
      display->draw_hline(x, y + 1, time - 1, color);
    }
    display->draw_vline(x + time - 1, y + 1, 3, color);
    if(time < 16)
    {
      display->draw_hline(x + time, y + 3, 16 - time, color);
    }
  }

  void display_clock(int16_t x, int16_t y, uint8_t bpm, DUINO_SSD1306::SSD1306Color color)
  {
    if(bpm == 0)
    {
      display->draw_text(x, y, "EXT", color);
    }
    else
    {
      display->draw_char(x, y, '0' + bpm / 10, color);
      display->draw_char(x + 6, y, '0' + bpm % 10, color);
      display->draw_char(x + 12, y, '0', color);
    }
  }

  void display_note(int16_t x, int16_t y, int8_t note, DUINO_SSD1306::SSD1306Color color)
  {
    // draw octave
    display->draw_char(x + 9, y + 7, '0' + note / 12, color);

    // draw note
    display->draw_char(x + 2, y + 4, semitone_lt[note % 12], color);
    
    // draw intonation symbol
    switch(semitone_in[note % 12])
    {
      case IF:
        display->draw_vline(x + 9, y + 1, 5, color);
        display->draw_pixel(x + 10, y + 3, color);
        display->draw_pixel(x + 10, y + 5, color);
        display->draw_vline(x + 11, y + 4, 2, color);
        break;
      case IS:
        display->draw_vline(x + 9, y + 1, 5, color);
        display->draw_pixel(x + 10, y + 2, color);
        display->draw_pixel(x + 10, y + 4, color);
        display->draw_vline(x + 11, y + 1, 5, color);
        break;
    }
  }

  void display_reverse_address(int16_t x, int16_t y)
  {
    display->fill_rect(x, y, 5, 7, DUINO_SSD1306::Black);

    if(seq_values.diradd_mode)
    {
      display->draw_char(30, 12, '1' + stage, DUINO_SSD1306::White);
    }
    else
    {
      display->draw_char(30, 12, 0x10 + (unsigned char)reverse, DUINO_SSD1306::White);
    }
  }

  void invert_current_selection()
  {
    switch(main_selected)
    {
      case 0: // save
        display->fill_rect(121, 0, 7, 7, DUINO_SSD1306::Inverse);
        display_changed[0] = true;
        break;
      case 1: // top
        switch(top_selected)
        {
          case 0: // count
            display->fill_rect(9, 11, 7, 9, DUINO_SSD1306::Inverse);
            break;
          case 1: // dir/add
            display->fill_rect(21, 11, 7, 9, DUINO_SSD1306::Inverse);
            break;
          case 2: // slew
            display->fill_rect(58, 11, 20, 9, DUINO_SSD1306::Inverse);
            break;
          case 3: // gate
            display->fill_rect(83, 11, 20, 9, DUINO_SSD1306::Inverse);
            break;
          case 4: // clock
            display->fill_rect(108, 11, 19, 9, DUINO_SSD1306::Inverse);
            break;
        }
        display_changed[1] = true;
        break;
      case 2: // pitch
        display->fill_rect(16 * stage_selected, 32, 16, 15, DUINO_SSD1306::Inverse);
        display_changed[3] = true;
        break;
      case 3: // steps
        display->fill_rect(16 * stage_selected, 48, 7, 9, DUINO_SSD1306::Inverse);
        display_changed[4] = true;
        break;
      case 4: // gate
        display->fill_rect(16 * stage_selected + 7, 48, 9, 9, DUINO_SSD1306::Inverse);
        display_changed[4] = true;
        break;
      case 5: // slew
        display->fill_rect(16 * stage_selected, 58, 16, 6, DUINO_SSD1306::Inverse);
        display_changed[5] = true;
        break;
    }
  }

  void display_gate(uint8_t stage, DUINO_SSD1306::SSD1306Color color)
  {
    display->fill_rect(16 * stage + 6, 26, 4, 4, color);
  }

  uint8_t main_selected;  // 0 - save, 1 - top, 2 - pitch, 3 - steps, 4 - gate, 5 - slew
  uint8_t top_selected;   // 0 - count, 1 - dir/add, 2 - slew, 3 - gate, 4 - clock
  uint8_t stage_selected;

  bool last_gate;
  uint8_t last_stage;
  bool last_diradd_mode;
  bool last_reverse;

  bool display_changed[6];

  struct DU_SEQ_Parameter_Values {
    int8_t stage_pitch[8];
    int8_t stage_steps[8];
    int8_t stage_gate[8];
    uint8_t stage_slew;
    int8_t stage_count;
    uint8_t diradd_mode;
    int8_t slew_rate;
    int8_t gate_time;
    int8_t clock_bpm;
  };

  union DU_SEQ_Parameters {
    DU_SEQ_Parameter_Values vals;
    uint8_t bytes[30];
  };

  DU_SEQ_Parameters params;
};

DU_SEQ_Function * function;
DU_SEQ_Interface * interface;

ENCODER_ISR(interface->encoder);

void clock_isr()
{
  clock_gate = seq_values.clock_ext ? function->gt_read_debounce(GT3) : !clock_gate;

  if(clock_gate)
  {
    step++;
    step %= seq_values.stage_steps[stage];
    if(!step)
    {
      stage = seq_values.diradd_mode ? function->address_to_stage() : (reverse ?
          (stage ? stage - 1 : seq_values.stage_count - 1) : stage + 1);
      stage %= seq_values.stage_count;
    }
    retrigger = true;
  }
}

void reset_isr()
{
  stage = step = 0;
  update_clock();
}

void update_clock()
{
  Timer1.detachInterrupt();
  if(!seq_values.clock_ext)
  {
    Timer1.attachInterrupt(clock_isr, seq_values.clock_period);
  }
}

void setup()
{
  stage = step = 0;
  gate = clock_gate = retrigger = false;

  Timer1.initialize();

  slew_filter = new DUINO_Filter(DUINO_Filter::LowPass, 1.0, 0.0);

  function = new DU_SEQ_Function();
  interface = new DU_SEQ_Interface();

  function->begin();
  interface->begin();
}

void loop()
{
  function->loop();
  interface->loop();
}
