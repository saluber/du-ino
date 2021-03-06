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
 * DU-INO ADSR Envelope & VCA Function
 * Aaron Mavrinac <aaron@logick.ca>
 */

#include <du-ino_function.h>
#include <du-ino_widgets.h>
#include <avr/pgmspace.h>

#define ENV_PEAK              10.0 // V
#define ENV_SATURATION        1.581976
#define ENV_DECAY_COEFF       -2.0
#define ENV_RELEASE_COEFF     -2.0
#define ENV_RELEASE_HOLD      3

#define V_MAX                 43

#define DEBOUNCE_MS           100 // ms

static const unsigned char icons[] PROGMEM = {
  0x38, 0x78, 0x7f, 0x7f, 0x7f, 0x78, 0x38  // gate
};

struct DU_ADSR_Values {
  uint16_t A;  // ms
  uint16_t D;  // ms
  float S;     // V
  uint16_t R;  // ms
};

volatile bool gate, retrigger;
volatile uint8_t selected_env;
volatile unsigned long debounce;

static const unsigned char label[4] = {'A', 'D', 'S', 'R'};

void gate_isr();
void switch_isr();

void save_click_callback();
void adsr_scroll_callback_0(int delta);
void adsr_scroll_callback_1(int delta);
void adsr_scroll_callback_2(int delta);
void adsr_scroll_callback_3(int delta);
void adsr_scroll_callback_4(int delta);
void adsr_scroll_callback_5(int delta);
void adsr_scroll_callback_6(int delta);
void adsr_scroll_callback_7(int delta);

class DU_ADSR_Function : public DUINO_Function {
 public:
  DU_ADSR_Function() : DUINO_Function(0b10111100) { }
  
  virtual void setup()
  {
    // build widget hierarchy
    container_outer_ = new DUINO_WidgetContainer<2>(DUINO_Widget::DoubleClick, 1);
    widget_save_ = new DUINO_DisplayWidget(121, 0, 7, 7, DUINO_Widget::Full);
    widget_save_->attach_click_callback(save_click_callback);
    container_outer_->attach_child(widget_save_, 0);
    container_adsr_ = new DUINO_WidgetContainer<8>(DUINO_Widget::Click);
    for (uint8_t i = 0; i < 4; ++i)
    {
      widgets_adsr_[i] = new DUINO_DisplayWidget(11 * i + 2, 55, 7, 9, DUINO_Widget::Full);
      widgets_adsr_[i + 4] = new DUINO_DisplayWidget(11 * i + 86, 55, 7, 9, DUINO_Widget::Full);
      container_adsr_->attach_child(widgets_adsr_[i], i);
      container_adsr_->attach_child(widgets_adsr_[i + 4], i + 4);
    }
    container_outer_->attach_child(container_adsr_, 1);
    widgets_adsr_[0]->attach_scroll_callback(adsr_scroll_callback_0);
    widgets_adsr_[1]->attach_scroll_callback(adsr_scroll_callback_1);
    widgets_adsr_[2]->attach_scroll_callback(adsr_scroll_callback_2);
    widgets_adsr_[3]->attach_scroll_callback(adsr_scroll_callback_3);
    widgets_adsr_[4]->attach_scroll_callback(adsr_scroll_callback_4);
    widgets_adsr_[5]->attach_scroll_callback(adsr_scroll_callback_5);
    widgets_adsr_[6]->attach_scroll_callback(adsr_scroll_callback_6);
    widgets_adsr_[7]->attach_scroll_callback(adsr_scroll_callback_7);

    env = 0;
    gate_time = 0;
    release_time = 0;
    last_selected_env = 0;
    last_gate = false;

    gt_attach_interrupt(GT3, gate_isr, CHANGE);
    gt_attach_interrupt(GT4, switch_isr, FALLING);

    // load/initialize ADSR values
    load_params(0, (uint8_t *)v, 8);
    for(uint8_t i = 0; i < 8; ++i)
    {
      if(v[i] < 0)
      {
        v[i] = 0;
      }
      else if(v[i] > V_MAX)
      {
        v[i] = V_MAX;
      }
      v_last[i] = v[i];
    }
    for(uint8_t e = 0; e < 2; ++e)
    {
      adsr_values[e].A = uint16_t(v[2 * e]) * 24;
      adsr_values[e].D = uint16_t(v[2 * e + 1]) * 24;
      adsr_values[e].S = (float(v[2 * e + 2]) / float(V_MAX)) * ENV_PEAK;
      adsr_values[e].R = uint16_t(v[2 * e + 3]) * 24;
    }

    // draw title
    Display.draw_du_logo_sm(0, 0, DUINO_SH1106::White);
    Display.draw_text(16, 0, "ADSR/VCA", DUINO_SH1106::White);

    // draw save box
    Display.fill_rect(122, 1, 5, 5, DUINO_SH1106::White);

    // draw envelope indicators
    Display.draw_char(47, 56, 0x11, DUINO_SH1106::White);
    Display.draw_char(54, 56, '1', DUINO_SH1106::White);
    Display.draw_char(69, 56, '2', DUINO_SH1106::White);
    Display.draw_char(76, 56, 0x10, DUINO_SH1106::White);
    Display.fill_rect(53, 55, 7, 9, DUINO_SH1106::Inverse);

    // draw sliders & labels
    for(uint8_t i = 0; i < 8; ++i)
    {
      Display.fill_rect(widgets_adsr_[i]->x() - 1, 51 - v[i], 9, 3, DUINO_SH1106::White);
      Display.draw_char(widgets_adsr_[i]->x() + 1, widgets_adsr_[i]->y() + 1, label[i % 4], DUINO_SH1106::White);
    }

    widget_setup(container_outer_);
    Display.display_all();
  }

  virtual void loop()
  {
    if(retrigger)
    {
      env = selected_env;
      gate_time = 0;
      release_time = 0;
      retrigger = false;
    }

    if(gate_time)
    {
      if(gate)
      {
        uint16_t elapsed = millis() - gate_time;
        if(elapsed < adsr_values[env].A)
        {
          // attack
          cv_current = ENV_PEAK * ENV_SATURATION * (1.0 - exp(-(float(elapsed) / float(adsr_values[env].A))));
        }
        else
        {
          // decay/sustain
          cv_current = adsr_values[env].S + (ENV_PEAK - adsr_values[env].S)
              * exp(ENV_DECAY_COEFF * (float(elapsed - adsr_values[env].A) / float(adsr_values[env].D)));
        }
      }
      else
      {
        if(release_time)
        {
          uint16_t elapsed = millis() - release_time;
          if(elapsed < adsr_values[env].R * ENV_RELEASE_HOLD)
          {
            // release
            cv_current = exp(ENV_RELEASE_COEFF * (float(elapsed) / float(adsr_values[env].R))) * cv_released;
          }
          else
          {
            cv_current = 0.0;
            release_time = 0;
            gate_time = 0;
          }
        }
        else
        {
          release_time = millis();
          cv_released = cv_current;
        }
      }

      cv_out(CO1, cv_current);
      cv_out(CO3, cv_current / 2.0);
    }
    else if(gate)
    {
      gate_time = millis()
          - (unsigned long)(-float(adsr_values[env].A) * log(1 - (cv_current / (ENV_PEAK * ENV_SATURATION))));
    }

    widget_loop();

    // display selected envelope
    if(selected_env != last_selected_env)
    {
      last_selected_env = selected_env;
      Display.fill_rect(53, 55, 7, 9, DUINO_SH1106::Inverse);
      Display.fill_rect(68, 55, 7, 9, DUINO_SH1106::Inverse);
      Display.display(53, 74, 6, 7);
    }

    // display gate
    if(gate != last_gate)
    {
      last_gate = gate;
      if(gate)
      {
        Display.draw_bitmap_7(60, 25, icons, 0, DUINO_SH1106::White);
      }
      else
      {
        Display.fill_rect(60, 25, 7, 7, DUINO_SH1106::Black);
      }
      Display.display(60, 66, 3, 3);
    }
  }

  void widget_save_click_callback()
  {
    if(!saved_)
    {
      save_params(0, v, 8);
      Display.fill_rect(widget_save_->x() + 2, widget_save_->y() + 2, 3, 3, DUINO_SH1106::Black);
      widget_save_->display();
    }
  }

  void widget_adsr_scroll_callback(uint8_t selected, int delta)
  {
    v[selected] += delta;
    if(v[selected] < 0)
    {
      v[selected] = 0;
    }
    if(v[selected] > V_MAX)
    {
      v[selected] = V_MAX;
    }
    if(v[selected] != v_last[selected])
    {
      // update slider
      Display.fill_rect(widgets_adsr_[selected]->x() - 1, 51 - v_last[selected], 9, 3, DUINO_SH1106::Black);
      Display.fill_rect(widgets_adsr_[selected]->x() - 1, 51 - v[selected], 9, 3, DUINO_SH1106::White);
      Display.display(widgets_adsr_[selected]->x() - 1, widgets_adsr_[selected]->x() + 7, 1, 6);

      // update ADSR value
      uint8_t e = selected > 3 ? 1 : 0;
      switch(selected % 4)
      {
        case 0:
          adsr_values[e].A = uint16_t(v[selected]) * 24;
          break;
        case 1:
          adsr_values[e].D = uint16_t(v[selected]) * 24;
          break;
        case 2:
          adsr_values[e].S = (float(v[selected]) / float(V_MAX)) * ENV_PEAK;
          break;
        case 3:
          adsr_values[e].R = uint16_t(v[selected]) * 24;
          break;
      }

      // update last encoder value
      v_last[selected] = v[selected];

      mark_save();
    }
  }

 private:
  void mark_save()
  {
    if(saved_)
    {
      saved_ = false;
      Display.fill_rect(widget_save_->x() + 2, widget_save_->y() + 2, 3, 3, DUINO_SH1106::Black);
      widget_save_->display();
    }
  }

  DU_ADSR_Values adsr_values[2];
  uint8_t env;
  unsigned long gate_time, release_time;
  float cv_current, cv_released;
  int8_t v[8], v_last[8];
  uint8_t last_selected_env;
  bool last_gate;

  DUINO_WidgetContainer<2> * container_outer_;
  DUINO_WidgetContainer<8> * container_adsr_;
  DUINO_DisplayWidget * widget_save_;
  DUINO_DisplayWidget * widgets_adsr_[8];
};

DU_ADSR_Function * function;

void gate_isr()
{
  gate = function->gt_read_debounce(DUINO_Function::GT3);
  if(gate)
    retrigger = true;
}

void switch_isr()
{
  if(millis() - debounce > DEBOUNCE_MS)
  {
    selected_env++;
    selected_env %= 2;
    debounce = millis();
  }
}

void save_click_callback() { function->widget_save_click_callback(); }
void adsr_scroll_callback_0(int delta) { function->widget_adsr_scroll_callback(0, delta); }
void adsr_scroll_callback_1(int delta) { function->widget_adsr_scroll_callback(1, delta); }
void adsr_scroll_callback_2(int delta) { function->widget_adsr_scroll_callback(2, delta); }
void adsr_scroll_callback_3(int delta) { function->widget_adsr_scroll_callback(3, delta); }
void adsr_scroll_callback_4(int delta) { function->widget_adsr_scroll_callback(4, delta); }
void adsr_scroll_callback_5(int delta) { function->widget_adsr_scroll_callback(5, delta); }
void adsr_scroll_callback_6(int delta) { function->widget_adsr_scroll_callback(6, delta); }
void adsr_scroll_callback_7(int delta) { function->widget_adsr_scroll_callback(7, delta); }

void setup()
{
  gate = retrigger = false;
  selected_env = 0;
  debounce = 0;

  function = new DU_ADSR_Function();

  function->begin();
}

void loop()
{
  function->loop();
}
