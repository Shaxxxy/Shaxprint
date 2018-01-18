/**
 * Marlin 3D Printer Firmware
 * Copyright (C) 2016 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (C) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/**
 * fwretract.cpp - Implement firmware-based retraction
 */

#include "MarlinConfig.h"

#if ENABLED(FWRETRACT)

#include "fwretract.h"
#include "Marlin.h"
#include "planner.h"
#include "stepper.h"

FWRetract fwretract; // Single instance - this calls the constructor

// private:

#if EXTRUDERS > 1
  bool FWRetract::retracted_swap[EXTRUDERS];         // Which extruders are swap-retracted
#endif

// public:

bool FWRetract::autoretract_enabled,                 // M209 S - Autoretract switch
     FWRetract::retracted[EXTRUDERS];                // Which extruders are currently retracted
float FWRetract::retract_length,                     // M207 S - G10 Retract length
      FWRetract::retract_feedrate_mm_s,              // M207 F - G10 Retract feedrate
      FWRetract::retract_zlift,                      // M207 Z - G10 Retract hop size
      FWRetract::retract_recover_length,             // M208 S - G11 Recover length
      FWRetract::retract_recover_feedrate_mm_s,      // M208 F - G11 Recover feedrate
      FWRetract::swap_retract_length,                // M207 W - G10 Swap Retract length
      FWRetract::swap_retract_recover_length,        // M208 W - G11 Swap Recover length
      FWRetract::swap_retract_recover_feedrate_mm_s; // M208 R - G11 Swap Recover feedrate

void FWRetract::reset() {
  autoretract_enabled = false;
  retract_length = RETRACT_LENGTH;
  retract_feedrate_mm_s = RETRACT_FEEDRATE;
  retract_zlift = RETRACT_ZLIFT;
  retract_recover_length = RETRACT_RECOVER_LENGTH;
  retract_recover_feedrate_mm_s = RETRACT_RECOVER_FEEDRATE;
  swap_retract_length = RETRACT_LENGTH_SWAP;
  swap_retract_recover_length = RETRACT_RECOVER_LENGTH_SWAP;
  swap_retract_recover_feedrate_mm_s = RETRACT_RECOVER_FEEDRATE_SWAP;

  for (uint8_t i = 0; i < EXTRUDERS; ++i) {
    retracted[i] = false;
    #if EXTRUDERS > 1
      retracted_swap[i] = false;
    #endif
  }
}

/**
 * Retract or recover according to firmware settings
 *
 * This function handles retract/recover moves for G10 and G11,
 * plus auto-retract moves sent from G0/G1 when E-only moves are done.
 *
 * To simplify the logic, doubled retract/recover moves are ignored.
 *
 * Note: Z lift is done transparently to the planner. Aborting
 *       a print between G10 and G11 may corrupt the Z position.
 *
 * Note: Auto-retract will apply the set Z hop in addition to any Z hop
 *       included in the G-code. Use M207 Z0 to to prevent double hop.
 */
void FWRetract::retract(const bool retracting
  #if EXTRUDERS > 1
    , bool swapping /* =false */
  #endif
) {

  static float hop_amount = 0.0;  // Total amount lifted, for use in recover

  // Prevent two retracts or recovers in a row
  if (retracted[active_extruder] == retracting) return;

  // Prevent two swap-retract or recovers in a row
  #if EXTRUDERS > 1
    // Allow G10 S1 only after G10
    if (swapping && retracted_swap[active_extruder] == retracting) return;
    // G11 priority to recover the long retract if activated
    if (!retracting) swapping = retracted_swap[active_extruder];
  #else
    const bool swapping = false;
  #endif

  /* // debugging
    SERIAL_ECHOLNPAIR("retracting ", retracting);
    SERIAL_ECHOLNPAIR("swapping ", swapping);
    SERIAL_ECHOLNPAIR("active extruder ", active_extruder);
    for (uint8_t i = 0; i < EXTRUDERS; ++i) {
      SERIAL_ECHOPAIR("retracted[", i);
      SERIAL_ECHOLNPAIR("] ", retracted[i]);
      SERIAL_ECHOPAIR("retracted_swap[", i);
      SERIAL_ECHOLNPAIR("] ", retracted_swap[i]);
    }
    SERIAL_ECHOLNPAIR("current_position[z] ", current_position[Z_AXIS]);
    SERIAL_ECHOLNPAIR("hop_amount ", hop_amount);
  //*/

  const bool has_zhop = retract_zlift > 0.01;     // Is there a hop set?
  const float old_feedrate_mm_s = feedrate_mm_s;

  // The current position will be the destination for E and Z moves
  set_destination_from_current();
  stepper.synchronize();  // Wait for buffered moves to complete

  const float renormalize = 1.0 / planner.e_factor[active_extruder];

  if (retracting) {
    // Retract by moving from a faux E position back to the current E position
    feedrate_mm_s = retract_feedrate_mm_s;
    current_position[E_AXIS] += (swapping ? swap_retract_length : retract_length) * renormalize;
    sync_plan_position_e();
    prepare_move_to_destination();

    // Is a Z hop set, and has the hop not yet been done?
    // No double zlifting
    // Feedrate to the max
    if (has_zhop && !hop_amount) {
      hop_amount += retract_zlift;                        // Carriage is raised for retraction hop
      feedrate_mm_s = planner.max_feedrate_mm_s[Z_AXIS];  // Z feedrate to max
      current_position[Z_AXIS] -= retract_zlift;          // Pretend current pos is lower. Next move raises Z.
      SYNC_PLAN_POSITION_KINEMATIC();                     // Set the planner to the new position
      prepare_move_to_destination();                      // Raise up to the old current pos
    }
  }
  else {
    // If a hop was done and Z hasn't changed, undo the Z hop
    if (hop_amount) {
      current_position[Z_AXIS] += retract_zlift;          // Pretend current pos is lower. Next move raises Z.
      SYNC_PLAN_POSITION_KINEMATIC();                     // Set the planner to the new position
      feedrate_mm_s = planner.max_feedrate_mm_s[Z_AXIS];  // Z feedrate to max
      prepare_move_to_destination();                      // Raise up to the old current pos
      hop_amount = 0.0;                                   // Clear hop
    }

    // A retract multiplier has been added here to get faster swap recovery
    feedrate_mm_s = swapping ? swap_retract_recover_feedrate_mm_s : retract_recover_feedrate_mm_s;

    const float move_e = swapping ? swap_retract_length + swap_retract_recover_length : retract_length + retract_recover_length;
    current_position[E_AXIS] -= move_e * renormalize;
    sync_plan_position_e();
    prepare_move_to_destination();  // Recover E
  }

  feedrate_mm_s = old_feedrate_mm_s;                      // Restore original feedrate

  retracted[active_extruder] = retracting;                // Active extruder now retracted / recovered

  // If swap retract/recover update the retracted_swap flag too
  #if EXTRUDERS > 1
    if (swapping) retracted_swap[active_extruder] = retracting;
  #endif

  /* // debugging
    SERIAL_ECHOLNPAIR("retracting ", retracting);
    SERIAL_ECHOLNPAIR("swapping ", swapping);
    SERIAL_ECHOLNPAIR("active_extruder ", active_extruder);
    for (uint8_t i = 0; i < EXTRUDERS; ++i) {
      SERIAL_ECHOPAIR("retracted[", i);
      SERIAL_ECHOLNPAIR("] ", retracted[i]);
      SERIAL_ECHOPAIR("retracted_swap[", i);
      SERIAL_ECHOLNPAIR("] ", retracted_swap[i]);
    }
    SERIAL_ECHOLNPAIR("current_position[z] ", current_position[Z_AXIS]);
    SERIAL_ECHOLNPAIR("hop_amount ", hop_amount);
  //*/

}

#endif // FWRETRACT
