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
#include <du-ino_widgets.h>
#include <du-ino_dsp.h>
#include <TimerOne.h>
#include <avr/pgmspace.h>

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
  0x00, 0x22, 0x14, 0x08, 0x14, 0x22, 0x00,  // off
  0x1C, 0x22, 0x5D, 0x5D, 0x5D, 0x22, 0x1C,  // single
  0x7C, 0x00, 0x7F, 0x00, 0x7F, 0x00, 0x7C,  // multi
  0x08, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x08,  // long
  0x30, 0x40, 0x52, 0x5F, 0x50, 0x40, 0x30,  // ext 1
  0x30, 0x40, 0x59, 0x55, 0x52, 0x40, 0x30   // ext 2
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

void clock_ext_isr();
void clock_isr();
void reset_isr();

void update_clock();

void save_click_callback();
void count_scroll_callback(int delta);
void diradd_scroll_callback(int delta);
void slew_scroll_callback(int delta);
void gate_scroll_callback(int delta);
void clock_scroll_callback(int delta);
void s_pitch_scroll_callback(uint8_t selected, int delta);
void s_steps_scroll_callback(uint8_t selected, int delta);
void s_gate_scroll_callback(uint8_t selected, int delta);
void s_slew_scroll_callback(uint8_t selected, int delta);
void s_pitch_click_callback();
void s_steps_click_callback();
void s_gate_click_callback();
void s_slew_click_callback();

class DU_SEQ_Function : public DUINO_Function {
 public:
  DU_SEQ_Function() : DUINO_Function(0b01111100) { }
  
  virtual void setup()
  {
    // build widget hierarchy
    container_outer_ = new DUINO_WidgetContainer<6>(DUINO_Widget::DoubleClick, 2);
    widget_save_ = new DUINO_DisplayWidget(121, 0, 7, 7, DUINO_Widget::Full);
    widget_save_->attach_click_callback(save_click_callback);
    container_outer_->attach_child(widget_save_, 0);
    container_top_ = new DUINO_WidgetContainer<5>(DUINO_Widget::Click);
    widget_count_ = new DUINO_DisplayWidget(9, 11, 7, 9, DUINO_Widget::Full);
    widget_count_->attach_scroll_callback(count_scroll_callback);
    container_top_->attach_child(widget_count_, 0);
    widget_diradd_ = new DUINO_DisplayWidget(21, 11, 7, 9, DUINO_Widget::Full);
    widget_diradd_->attach_scroll_callback(diradd_scroll_callback);
    container_top_->attach_child(widget_diradd_, 1);
    widget_slew_ = new DUINO_DisplayWidget(58, 11, 20, 9, DUINO_Widget::Full);
    widget_slew_->attach_scroll_callback(slew_scroll_callback);
    container_top_->attach_child(widget_slew_, 2);
    widget_gate_ = new DUINO_DisplayWidget(83, 11, 20, 9, DUINO_Widget::Full);
    widget_gate_->attach_scroll_callback(gate_scroll_callback);
    container_top_->attach_child(widget_gate_, 3);
    widget_clock_ = new DUINO_DisplayWidget(108, 11, 19, 9, DUINO_Widget::Full);
    widget_clock_->attach_scroll_callback(clock_scroll_callback);
    container_top_->attach_child(widget_clock_, 4);
    container_outer_->attach_child(container_top_, 1);
    widgets_pitch_ = new DUINO_MultiDisplayWidget<8>(0, 32, 16, 15, 16, false, DUINO_Widget::Full, DUINO_Widget::Click);
    widgets_pitch_->attach_scroll_callback_array(s_pitch_scroll_callback);
    widgets_pitch_->attach_click_callback(s_pitch_click_callback);
    container_outer_->attach_child(widgets_pitch_, 2);
    widgets_steps_ = new DUINO_MultiDisplayWidget<8>(0, 48, 7, 9, 16, false, DUINO_Widget::Full, DUINO_Widget::Click);
    widgets_steps_->attach_scroll_callback_array(s_steps_scroll_callback);
    widgets_steps_->attach_click_callback(s_steps_click_callback);
    container_outer_->attach_child(widgets_steps_, 3);
    widgets_gate_ = new DUINO_MultiDisplayWidget<8>(7, 48, 9, 9, 16, false, DUINO_Widget::Full, DUINO_Widget::Click);
    widgets_gate_->attach_scroll_callback_array(s_gate_scroll_callback);
    widgets_gate_->attach_click_callback(s_gate_click_callback);
    container_outer_->attach_child(widgets_gate_, 4);
    widgets_slew_ = new DUINO_MultiDisplayWidget<8>(0, 58, 16, 6, 16, false, DUINO_Widget::Full, DUINO_Widget::Click);
    widgets_slew_->attach_scroll_callback_array(s_slew_scroll_callback);
    widgets_slew_->attach_click_callback(s_slew_click_callback);
    container_outer_->attach_child(widgets_slew_, 5);

    last_gate = false;
    last_stage = 0;
    last_diradd_mode = false;
    last_reverse = false;
    ext_clock_received = false;

    gt_attach_interrupt(GT3, clock_ext_isr, CHANGE);
    gt_attach_interrupt(GT4, reset_isr, FALLING);

    // draw top line
    Display.draw_du_logo_sm(0, 0, DUINO_SH1106::White);
    Display.draw_text(16, 0, "SEQ", DUINO_SH1106::White);

    // draw save box
    Display.fill_rect(widget_save_->x() + 1, widget_save_->y() + 1, 5, 5, DUINO_SH1106::White);

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
      Display.draw_vline(2 + i, 18 - i, i + 1, DUINO_SH1106::White);
    }
    Display.draw_char(widget_count_->x() + 1, widget_count_->y() + 1,
        '0' + params.vals.stage_count, DUINO_SH1106::White);
    Display.draw_char(widget_diradd_->x() + 1, widget_diradd_->y() + 1,
        params.vals.diradd_mode ? 'A' : 'D', DUINO_SH1106::White);
    Display.draw_char(30, 12, 0x10, DUINO_SH1106::White);

    Display.draw_vline(widget_slew_->x() + 1, widget_slew_->y() + 1, 7, DUINO_SH1106::White);
    Display.draw_hline(widget_slew_->x() + 2, widget_slew_->y() + 1, 16, DUINO_SH1106::White);
    Display.draw_hline(widget_slew_->x() + 2, widget_slew_->y() + 7, 16, DUINO_SH1106::White);
    Display.draw_vline(widget_slew_->x() + widget_slew_->width() - 2, widget_slew_->y() + 1, 7, DUINO_SH1106::White);
    display_slew_rate(widget_slew_->x() + 2, widget_slew_->y() + 2, params.vals.slew_rate, DUINO_SH1106::White);

    Display.draw_vline(widget_gate_->x() + 1, widget_gate_->y() + 1, 7, DUINO_SH1106::White);
    Display.draw_hline(widget_gate_->x() + 2, widget_gate_->y() + 1, 16, DUINO_SH1106::White);
    Display.draw_hline(widget_gate_->x() + 2, widget_gate_->y() + 7, 16, DUINO_SH1106::White);
    Display.draw_vline(widget_gate_->x() + widget_gate_->width() - 2, widget_slew_->y() + 1, 7, DUINO_SH1106::White);   
    display_gate_time(widget_gate_->x() + 2, widget_gate_->y() + 2, params.vals.gate_time, DUINO_SH1106::White);

    display_clock(widget_clock_->x() + 1, widget_clock_->y() + 1, params.vals.clock_bpm, DUINO_SH1106::White);

    // draw step elements
    for(uint8_t i = 0; i < 8; ++i)
    {
      // pitch
      display_note(widgets_pitch_->x(i), widgets_pitch_->y(i), params.vals.stage_pitch[i], DUINO_SH1106::White);
      // steps
      Display.draw_char(widgets_steps_->x(i) + 1, widgets_steps_->y(i) + 1,
          '0' + params.vals.stage_steps[i], DUINO_SH1106::White);
      // gate mode
      Display.draw_bitmap_7(widgets_gate_->x(i) + 1, widgets_gate_->y(i) + 1,
          gate_mode_icons, (GateMode)params.vals.stage_gate[i], DUINO_SH1106::White);
      // slew
      Display.fill_rect(widgets_slew_->x(i) + 1, widgets_slew_->y(i) + 1, 14, 4, DUINO_SH1106::White);
      Display.fill_rect(widgets_slew_->x(i) + 2 + 6 * (~(params.vals.stage_slew >> i) & 1),
          widgets_slew_->y(i) + 2, 6, 2, DUINO_SH1106::Black);
    }

    widget_setup(container_outer_);
    Display.display_all();
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
      // drop gate at start of stage
      if(!cached_step)
      {
        gt_out(GT1, false);
      }

      // drop clock each step
      gt_out(GT2, false);

      // update step clock time
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
        gate = gt_read(CI2);
        break;
      case GATE_EXT2:
        gate = gt_read(CI3);
        break;
    }

    // set pitch CV state
    float slew_cv = slew_filter->filter(seq_values.stage_cv[cached_stage]);
    cv_out(CO1, seq_values.stage_slew[cached_stage] ? slew_cv : seq_values.stage_cv[cached_stage]);

    // set gate and clock states
    gt_out(GT1, gate);
    gt_out(GT2, cached_clock_gate);

    // update reverse setting
    if(!seq_values.diradd_mode)
    {
      reverse = gt_read(CI1);
    }

    widget_loop();

    // display reverse/address
    if(seq_values.diradd_mode != last_diradd_mode
        || (!seq_values.diradd_mode && reverse != last_reverse)
        || (seq_values.diradd_mode && stage != last_stage))
    {
      display_reverse_address(30, 12);
      last_diradd_mode = seq_values.diradd_mode;
      last_reverse = reverse;
      Display.display(30, 34, 1, 2);
    }

    // display gate
    if(gate != last_gate || stage != last_stage)
    {
      uint8_t last_stage_cached = last_stage;
      last_gate = gate;
      last_stage = stage;

      if(gate)
      {
        if(stage != last_stage_cached)
        {
          display_gate(last_stage_cached, DUINO_SH1106::Black);
        }
        display_gate(stage, DUINO_SH1106::White);
      }
      else
      {
        display_gate(last_stage_cached, DUINO_SH1106::Black);
      }

      Display.display(16 * last_stage_cached + 6, 16 * last_stage_cached + 9, 3, 3);
      Display.display(16 * stage + 6, 16 * stage + 9, 3, 3);
    }

    // update clock from input
    if(ext_clock_received)
    {
      Display.fill_rect(widget_clock_->x() + 1, widget_clock_->y() + 1, 17, 7,
          widget_clock_->inverted() ? DUINO_SH1106::White : DUINO_SH1106::Black);
      display_clock(widget_clock_->x() + 1, widget_clock_->y() + 1, params.vals.clock_bpm,
          widget_clock_->inverted() ? DUINO_SH1106::Black : DUINO_SH1106::White);
      widget_clock_->display();
      ext_clock_received = false;
    }
  }

  void widget_save_click_callback()
  {
    if(!saved_)
    {
      save_params(0, params.bytes, 30);
      Display.fill_rect(widget_save_->x() + 2, widget_save_->y() + 2, 3, 3, DUINO_SH1106::Black);
      widget_save_->display();
    }
  }

  void widget_count_scroll_callback(int delta)
  {
    params.vals.stage_count += delta;
    if(params.vals.stage_count < 1)
    {
      params.vals.stage_count = 1;
    }
    else if(params.vals.stage_count > 8)
    {
      params.vals.stage_count = 8;
    }
    seq_values.stage_count = params.vals.stage_count;
    mark_save();
    Display.fill_rect(widget_count_->x() + 1, widget_count_->y() + 1, 5, 7, DUINO_SH1106::White);
    Display.draw_char(widget_count_->x() + 1, widget_count_->y() + 1, '0' + params.vals.stage_count,
        DUINO_SH1106::Black);
    widget_count_->display();
  }

  void widget_diradd_scroll_callback(int delta)
  {
    if(delta < 0)
    {
      params.vals.diradd_mode = 0;
    }
    else if(delta > 0)
    {
      params.vals.diradd_mode = 1;
    }
    seq_values.diradd_mode = (bool)params.vals.diradd_mode;
    mark_save();
    Display.fill_rect(widget_diradd_->x() + 1, widget_diradd_->y() + 1, 5, 7, DUINO_SH1106::White);
    Display.draw_char(widget_diradd_->x() + 1, widget_diradd_->y() + 1, params.vals.diradd_mode ? 'A' : 'D',
        DUINO_SH1106::Black);
    widget_diradd_->display();
  }

  void widget_slew_scroll_callback(int delta)
  {
    params.vals.slew_rate += delta;
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
    mark_save();
    Display.fill_rect(widget_slew_->x() + 2, widget_slew_->y() + 2, 16, 5, DUINO_SH1106::White);
    display_slew_rate(widget_slew_->x() + 2, widget_slew_->y() + 2, params.vals.slew_rate, DUINO_SH1106::Black);
    widget_slew_->display();
  }

  void widget_gate_scroll_callback(int delta)
  {
    params.vals.gate_time += delta;
    if(params.vals.gate_time < 0)
    {
      params.vals.gate_time = 0;
    }
    else if(params.vals.gate_time > 16)
    {
      params.vals.gate_time = 16;
    }
    seq_values.gate_ms = params.vals.gate_time * (uint16_t)(seq_values.clock_period / 8000);
    mark_save();
    Display.fill_rect(widget_gate_->x() + 2, widget_gate_->y() + 2, 16, 5, DUINO_SH1106::White);
    display_gate_time(widget_gate_->x() + 2, widget_gate_->y() + 2, params.vals.gate_time, DUINO_SH1106::Black);
    widget_gate_->display();
  }

  void widget_clock_scroll_callback(int delta)
  {
    params.vals.clock_bpm += delta;
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
    mark_save();
    Display.fill_rect(widget_clock_->x() + 1, widget_clock_->y() + 1, 17, 7, DUINO_SH1106::White);
    display_clock(widget_clock_->x() + 1, widget_clock_->y() + 1, params.vals.clock_bpm, DUINO_SH1106::Black);
    update_clock();
    widget_clock_->display();
  }

  void widgets_pitch_scroll_callback(uint8_t stage_selected, int delta)
  {
    params.vals.stage_pitch[stage_selected] += delta;
    if(params.vals.stage_pitch[stage_selected] < 0)
    {
      params.vals.stage_pitch[stage_selected] = 0;
    }
    else if(params.vals.stage_pitch[stage_selected] > 119)
    {
      params.vals.stage_pitch[stage_selected] = 119;
    }
    seq_values.stage_cv[stage_selected] = note_to_cv(params.vals.stage_pitch[stage_selected]);
    mark_save();
    Display.fill_rect(widgets_pitch_->x(stage_selected), widgets_pitch_->y(stage_selected), 16, 15,
        DUINO_SH1106::White);
    display_note(widgets_pitch_->x(stage_selected), widgets_pitch_->y(stage_selected),
        params.vals.stage_pitch[stage_selected], DUINO_SH1106::Black);
    widgets_pitch_->display();
  }

  void widgets_steps_scroll_callback(uint8_t stage_selected, int delta)
  {
    params.vals.stage_steps[stage_selected] += delta;
    if(params.vals.stage_steps[stage_selected] < 1)
    {
      params.vals.stage_steps[stage_selected] = 1;
    }
    else if(params.vals.stage_steps[stage_selected] > 8)
    {
      params.vals.stage_steps[stage_selected] = 8;
    }
    seq_values.stage_steps[stage_selected] = params.vals.stage_steps[stage_selected];
    mark_save();
    Display.fill_rect(widgets_steps_->x(stage_selected) + 1, widgets_steps_->y(stage_selected) + 1, 5, 7,
        DUINO_SH1106::White);
    Display.draw_char(widgets_steps_->x(stage_selected) + 1, widgets_steps_->y(stage_selected) + 1,
        '0' + params.vals.stage_steps[stage_selected], DUINO_SH1106::Black);
    widgets_steps_->display();
  }

  void widgets_gate_scroll_callback(uint8_t stage_selected, int delta)
  {
    params.vals.stage_gate[stage_selected] += delta;
    if(params.vals.stage_gate[stage_selected] < 0)
    {
      params.vals.stage_gate[stage_selected] = 0;
    }
    else if(params.vals.stage_gate[stage_selected] > 5)
    {
      params.vals.stage_gate[stage_selected] = 5;
    }
    seq_values.stage_gate[stage_selected] = (GateMode)params.vals.stage_gate[stage_selected];
    mark_save();
    Display.fill_rect(widgets_gate_->x(stage_selected) + 1, widgets_gate_->y(stage_selected) + 1, 7, 7,
        DUINO_SH1106::White);
    Display.draw_bitmap_7(widgets_gate_->x(stage_selected) + 1, widgets_gate_->y(stage_selected) + 1,
        gate_mode_icons, (GateMode)params.vals.stage_gate[stage_selected], DUINO_SH1106::Black);
    widgets_gate_->display();
  }

  void widgets_slew_scroll_callback(uint8_t stage_selected, int delta)
  {
    if(delta < 0)
    {
      params.vals.stage_slew &= ~(1 << stage_selected);
    }
    else if(delta > 0)
    {
      params.vals.stage_slew |= (1 << stage_selected);
    }
    seq_values.stage_slew[stage_selected] = (bool)((params.vals.stage_slew >> stage_selected) & 1);
    mark_save();
    Display.fill_rect(widgets_slew_->x(stage_selected) + 1, widgets_slew_->y(stage_selected) + 1, 14, 4,
        DUINO_SH1106::Black);
    Display.fill_rect(widgets_slew_->x(stage_selected) + 2 + 6 * (~(params.vals.stage_slew >> stage_selected) & 1),
        widgets_slew_->y(stage_selected) + 2, 6, 2, DUINO_SH1106::White);
    widgets_slew_->display();
  }

  void widgets_pitch_click_callback()
  {
    widgets_steps_->select(widgets_pitch_->selected());
    widgets_gate_->select(widgets_pitch_->selected());
    widgets_slew_->select(widgets_pitch_->selected());
  }

  void widgets_steps_click_callback()
  {
    widgets_pitch_->select(widgets_steps_->selected());
    widgets_gate_->select(widgets_steps_->selected());
    widgets_slew_->select(widgets_steps_->selected());
  }

  void widgets_gate_click_callback()
  {
    widgets_pitch_->select(widgets_gate_->selected());
    widgets_steps_->select(widgets_gate_->selected());
    widgets_slew_->select(widgets_gate_->selected());
  }

  void widgets_slew_click_callback()
  {
    widgets_pitch_->select(widgets_slew_->selected());
    widgets_steps_->select(widgets_slew_->selected());
    widgets_gate_->select(widgets_slew_->selected());
  }

  void set_clock_ext()
  {
    params.vals.clock_bpm = 0;
    seq_values.clock_ext = true;
    ext_clock_received = true;
    update_clock();
  }

  uint8_t address_to_stage()
  {
    int8_t addr_stage = (int8_t)(cv_read(CI1) * 1.6);
    if(addr_stage < 0)
    {
      addr_stage = 0;
    }
    else if(stage > seq_values.stage_count - 1)
    {
      addr_stage = seq_values.stage_count - 1;
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

  void display_slew_rate(int16_t x, int16_t y, uint8_t rate, DUINO_SH1106::SH1106Color color)
  {
    Display.draw_vline(x + rate - 1, y, 5, color);
  }

  void display_gate_time(int16_t x, int16_t y, uint8_t time, DUINO_SH1106::SH1106Color color)
  {
    if(time > 1)
    {
      Display.draw_hline(x, y + 1, time - 1, color);
    }
    Display.draw_vline(x + time - 1, y + 1, 3, color);
    if(time < 16)
    {
      Display.draw_hline(x + time, y + 3, 16 - time, color);
    }
  }

  void display_clock(int16_t x, int16_t y, uint8_t bpm, DUINO_SH1106::SH1106Color color)
  {
    if(bpm == 0)
    {
      Display.draw_text(x, y, "EXT", color);
    }
    else
    {
      Display.draw_char(x, y, '0' + bpm / 10, color);
      Display.draw_char(x + 6, y, '0' + bpm % 10, color);
      Display.draw_char(x + 12, y, '0', color);
    }
  }

  void display_note(int16_t x, int16_t y, int8_t note, DUINO_SH1106::SH1106Color color)
  {
    // draw octave
    Display.draw_char(x + 9, y + 7, '0' + note / 12, color);

    // draw note
    Display.draw_char(x + 2, y + 4, semitone_lt[note % 12], color);
    
    // draw intonation symbol
    switch(semitone_in[note % 12])
    {
      case IF:
        Display.draw_vline(x + 9, y + 1, 5, color);
        Display.draw_pixel(x + 10, y + 3, color);
        Display.draw_pixel(x + 10, y + 5, color);
        Display.draw_vline(x + 11, y + 4, 2, color);
        break;
      case IS:
        Display.draw_vline(x + 9, y + 1, 5, color);
        Display.draw_pixel(x + 10, y + 2, color);
        Display.draw_pixel(x + 10, y + 4, color);
        Display.draw_vline(x + 11, y + 1, 5, color);
        break;
    }
  }

  void display_reverse_address(int16_t x, int16_t y)
  {
    Display.fill_rect(x, y, 5, 7, DUINO_SH1106::Black);

    if(seq_values.diradd_mode)
    {
      Display.draw_char(30, 12, '1' + stage, DUINO_SH1106::White);
    }
    else
    {
      Display.draw_char(30, 12, 0x10 + (unsigned char)reverse, DUINO_SH1106::White);
    }
  }

  void display_gate(uint8_t stage, DUINO_SH1106::SH1106Color color)
  {
    Display.fill_rect(16 * stage + 6, 26, 4, 4, color);
  }

  void mark_save()
  {
    if(saved_)
    {
      saved_ = false;
      Display.fill_rect(widget_save_->x() + 2, widget_save_->y() + 2, 3, 3, DUINO_SH1106::Black);
      widget_save_->display();
    }
  }

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

  DUINO_WidgetContainer<6> * container_outer_;
  DUINO_WidgetContainer<5> * container_top_;
  DUINO_DisplayWidget * widget_save_;
  DUINO_DisplayWidget * widget_count_;
  DUINO_DisplayWidget * widget_diradd_;
  DUINO_DisplayWidget * widget_slew_;
  DUINO_DisplayWidget * widget_gate_;
  DUINO_DisplayWidget * widget_clock_;
  DUINO_MultiDisplayWidget<8> * widgets_pitch_;
  DUINO_MultiDisplayWidget<8> * widgets_steps_;
  DUINO_MultiDisplayWidget<8> * widgets_gate_;
  DUINO_MultiDisplayWidget<8> * widgets_slew_;

  uint8_t cached_stage, cached_step;
  bool cached_clock_gate, cached_retrigger;
  unsigned long clock_time;

  bool last_gate;
  uint8_t last_stage;
  bool last_diradd_mode;
  bool last_reverse;

  bool ext_clock_received;
};

DU_SEQ_Function * function;

void clock_ext_isr()
{
  if(!seq_values.clock_ext)
  {
    function->set_clock_ext();
  }

  clock_isr();
}

void clock_isr()
{
  clock_gate = seq_values.clock_ext ? function->gt_read_debounce(DUINO_Function::GT3) : !clock_gate;

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

void save_click_callback() { function->widget_save_click_callback(); }
void count_scroll_callback(int delta) { function->widget_count_scroll_callback(delta); }
void diradd_scroll_callback(int delta) { function->widget_diradd_scroll_callback(delta); }
void slew_scroll_callback(int delta) { function->widget_slew_scroll_callback(delta); }
void gate_scroll_callback(int delta) { function->widget_gate_scroll_callback(delta); }
void clock_scroll_callback(int delta) { function->widget_clock_scroll_callback(delta); }
void s_pitch_scroll_callback(uint8_t selected, int delta) { function->widgets_pitch_scroll_callback(selected, delta); }
void s_steps_scroll_callback(uint8_t selected, int delta) { function->widgets_steps_scroll_callback(selected, delta); }
void s_gate_scroll_callback(uint8_t selected, int delta) { function->widgets_gate_scroll_callback(selected, delta); }
void s_slew_scroll_callback(uint8_t selected, int delta) { function->widgets_slew_scroll_callback(selected, delta); }
void s_pitch_click_callback() { function->widgets_pitch_click_callback(); }
void s_steps_click_callback() { function->widgets_steps_click_callback(); }
void s_gate_click_callback() { function->widgets_gate_click_callback(); }
void s_slew_click_callback() { function->widgets_slew_click_callback(); }

void setup()
{
  stage = step = 0;
  gate = clock_gate = retrigger = false;

  Timer1.initialize();

  slew_filter = new DUINO_Filter(DUINO_Filter::LowPass, 1.0, 0.0);

  function = new DU_SEQ_Function();

  function->begin();
}

void loop()
{
  function->loop();
}
