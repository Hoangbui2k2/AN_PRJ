#ifndef PUMP_H
#define PUMP_H

#include <stdint.h>
#include <stdbool.h>

/* Pin definition (from AGENT.md) */
#define PUMP_TOGGLE_GPIO    23

/* CD4013 toggle timing */
#define PUMP_PULSE_MS       20   /* 20ms pulse to trigger CD4013 */
#define PUMP_DEBOUNCE_MS    50   /* Debounce delay for stability */

/* CD4013 power-on state (hardware-specific).
 *
 * Hardware: CLOCK1=GPIO23, SET1/RESET1=GND, D1=Q̅1 (toggle flip-flop),
 * Q1 -> AO3400A MOSFET (active-high) -> pump.  Q1 toggles on each rising
 * edge of CLOCK1 and is HELD across deep sleep.
 *
 * This unit's CD4013 powers up with Q1 LOW, so the physical pump starts OFF
 * at power-on.  The firmware must assume this on cold boot — if it assumes
 * ON instead, every toggle is inverted (pump_on actually turns the pump OFF
 * and pump_off turns it ON).  Set to 1 only if a different unit powers up
 * with Q1 HIGH (pump starts ON).
 */
#define PUMP_COLDBOOT_STATE 0

/**
 * @brief Initialize pump control GPIO
 */
void pump_init(void);

/**
 * @brief Toggle pump state (CD4013 flip-flop)
 * @return true if successful, false if toggle failed
 */
bool pump_toggle(void);

/**
 * @brief Force pump ON
 * @return true on success
 */
bool pump_on(void);

/**
 * @brief Force pump OFF
 * @return true on success
 */
bool pump_off(void);

/**
 * @brief Get current pump state
 * @return 1=ON, 0=OFF
 */
uint8_t pump_get_state(void);

/**
 * @brief Verify pump state matches expected by reading the toggle GPIO
 * @return true if state matches expected
 */
bool pump_verify_state(void);

#endif /* PUMP_H */
