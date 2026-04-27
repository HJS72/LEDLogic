# V2 Variables System - UI Specification

## JSON Script Format with Variables

### Example Script with Variables
```json
{
  "loop": true,
  "vars": [
    {"name":"my_brightness","type":"brightness","value":150},
    {"name":"step_duration","type":"duration","value":1000},
    {"name":"selected_leds","type":"led_mask","value":15}
  ],
  "ops": [
    {"op":"set_var","name":"my_brightness","type":"brightness","value":200},
    {"op":"set","leds":"0,1,2,3","color":"#ff0000","br":"$my_brightness"},
    {"op":"wait","s":"$step_duration"},
    {"op":"change_var","name":"my_brightness","op_type":"subtract","value":50},
    {"op":"set","leds":"$selected_leds","color":"#00ff00","br":"$my_brightness"},
    {"op":"wait","s":2}
  ]
}
```

## UI Components to Implement

### 1. LED Selection Component
- **Visual LED Strip Preview**: 12 LED boxes, clickable for selection
- **Checkboxes**: Alternative list of LEDs with enable/disable
- **Quick Actions**: "All", "None" buttons
- **Variable Option**: Dropdown to select from defined variables of type `led_mask`

HTML Structure:
```html
<div class="led-selector">
  <h4>LED-Auswahl</h4>
  <div class="led-strip-preview">
    <div class="led" data-index="0"></div>
    <div class="led" data-index="1"></div>
    <!-- ... 12 total -->
  </div>
  <div class="led-actions">
    <button>Alle auswählen</button>
    <button>Keine auswählen</button>
  </div>
  <select name="led-variable">
    <option value="">Wert eingeben</option>
    <option value="$selected_leds">Variable: selected_leds</option>
  </select>
</div>
```

### 2. Brightness Control Component
- **Slider**: 0-255 range
- **Value Display**: Shows current value
- **Variable Option**: Dropdown to select from brightness variables
- **Quick Presets**: Buttons for 0%, 50%, 100%

HTML Structure:
```html
<div class="brightness-control">
  <label>Helligkeit</label>
  <div class="brightness-input">
    <input type="range" min="0" max="255" value="128">
    <span class="value-display">128</span>
  </div>
  <div class="quick-presets">
    <button data-value="0">0%</button>
    <button data-value="128">50%</button>
    <button data-value="255">100%</button>
  </div>
  <select name="brightness-variable">
    <option value="">Wert eingeben</option>
    <option value="$my_brightness">Variable: my_brightness</option>
  </select>
</div>
```

### 3. Duration Input Component
- **Seconds Input**: Main input field
- **ms Fine-tuning**: Optional milliseconds
- **Variable Option**: Dropdown
- **Quick Presets**: 100ms, 500ms, 1000ms, 2000ms, 5000ms buttons

HTML Structure:
```html
<div class="duration-control">
  <label>Dauer</label>
  <div class="duration-input">
    <input type="number" name="duration_s" min="0" max="30" placeholder="0" style="width:60px;">
    <span>s</span>
    <input type="number" name="duration_ms" min="0" max="999" placeholder="000" style="width:50px;">
    <span>ms</span>
  </div>
  <div class="quick-presets">
    <button data-ms="100">100ms</button>
    <button data-ms="500">500ms</button>
    <button data-ms="1000">1s</button>
    <button data-ms="2000">2s</button>
    <button data-ms="5000">5s</button>
  </div>
  <select name="duration-variable">
    <option value="">Wert eingeben</option>
    <option value="$step_duration">Variable: step_duration</option>
  </select>
</div>
```

### 4. Variable Creation Panel
**Location**: Top of Script Editor or in dedicated tab

```html
<div class="action-panel variable-creation">
  <h4>Neue Variable</h4>
  <input type="text" name="var_name" placeholder="Variable Name (z.B. 'my_brightness')" maxlength="15">
  
  <select name="var_type">
    <option value="brightness">Helligkeit (0-255)</option>
    <option value="duration">Dauer (Millisekunden)</option>
    <option value="led_mask">LED-Auswahl (Bitmask)</option>
    <option value="color">Farbe (RGB Hex)</option>
  </select>
  
  <div id="var_input_dynamic">
    <!-- Populated based on selected type -->
  </div>
  
  <button class="primary">Variable erstellen</button>
</div>
```

### 5. Script Step Editor with Variables

**Show in step preview**:
```html
<div class="script-step-display">
  <span class="step-number">1</span>
  <span class="step-action">Set Variable</span>
  <code class="step-detail">my_brightness = 200</code>
</div>

<div class="script-step-display">
  <span class="step-number">2</span>
  <span class="step-action">Set Color</span>
  <code class="step-detail">LEDs 0-3: RGB(255,0,0), Brightness: $my_brightness</code>
</div>
```

## CSS Classes to Add

```css
.led-selector { /* container for LED selection */ }
.led-strip-preview { display: flex; gap: 6px; }
.led { width: 24px; height: 24px; border-radius: 4px; cursor: pointer; border: 2px solid #ddd; }
.led.selected { border-color: #9b3db8; background: #9b3db8; }
.led-actions { display: flex; gap: 8px; margin-top: 10px; }

.brightness-control { /* container */ }
.brightness-input { display: flex; align-items: center; gap: 10px; }
.value-display { min-width: 40px; text-align: right; font-weight: bold; }

.duration-control { /* container */ }
.duration-input { display: flex; align-items: center; gap: 8px; margin-bottom: 10px; }

.quick-presets { display: flex; gap: 6px; flex-wrap: wrap; }
.quick-presets button { flex: 1 1 auto; padding: 8px; font-size: 0.85rem; }

.action-panel { background: #f9f7fc; border-radius: 12px; padding: 16px; margin-bottom: 16px; }
.variable-creation { border: 2px dashed #9b3db8; }

.step-detail { background: #f2eaff; color: #3d2456; padding: 4px 8px; border-radius: 4px; }
```

## JavaScript Event Handlers Needed

1. LED selection toggle (visual + mask calculation)
2. Brightness slider real-time update
3. Duration input validation (0-30000 ms)
4. Variable type selector changes (update input fields)
5. Variable creation submission
6. Script preview update on variable changes

## Backend API Endpoints (Future)

- `GET /variables` - Get all defined variables in current script
- `POST /variables` - Create/update variable
- `DELETE /variables/{name}` - Delete variable
- `POST /preview/variables` - Test variable substitution in preview
