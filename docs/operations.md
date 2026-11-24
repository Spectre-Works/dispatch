<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Operate DISPATCH/01

Open the controller's LAN address and unlock it with the owner key. For a new
development unit, start with [USB pairing and Wi-Fi setup](development.md#provision-the-controller).
Use **Lock browser** on a shared device to clear its saved sign-in.

## Pair equipment and enable control

1. Connect a supported outlet to the trusted LAN. In **Settings →
   Equipment & plan → Pair equipment**, choose a slot and select **Use**
   for a discovered Shelly, or enter the address and ID for the selected
   profile. Add the miner IPv4 address for cgminer telemetry on TCP 4028.
   [Integration setup and credentials](integration-setup.md#pair-an-outlet)
2. Select **Save pairing in Preview**. Under **Equipment → First-run checks**,
   verify the physical outlet-to-miner mapping using a fresh outlet reading.
   Repeat this when an association changes. Give the equipment a name in
   **Equipment** so its actions are easy to recognize.
3. Open **Settings → Equipment & plan → Operating plan** or use **Edit plan** on the Overview.
   Set the plan, select **Review operating plan**, then **Save and preview**.
   The review shows example scenarios. **Outlet Preview** on Overview puts
   each outlet's reported state and power next to the proposed action and its reason.
   Select an equipment name to inspect that miner's readings, evidence, and controls.
4. Select **Enable outlet control** below the outlet decisions after reviewing them.
   When safe to start a miner with unknown draw, use **Equipment → First-run
   power test** on a mapped, powered-off outlet that supports the timed test. The test can use part of
   your allowance. It runs the outlet for up to four minutes, confirms it is
   off, and records the peak and steady draw. You can instead enter a known
   startup estimate for the selected miner under **Settings → Equipment & plan → Pair equipment**.

**Return to Preview** below **Outlet decisions** suspends new automation
requests and leaves outlets at their observed states. With outlet control
enabled, **Pause operation** requests that outlets turn off. You can pause
for a set duration or until you resume. The indefinite pause survives a restart.

## Set the operating plan

| Setting | What it controls |
| --- | --- |
| **Allowed mining hours** | When miners may run on any power source. Choose anytime or enter local hours, including an overnight window. |
| **Quiet Hours** | Stops mining and blocks starts during the chosen hours. |
| **Allow paid mining** | Lets DISPATCH buy power to start eligible miners and sustain running miners during the paid window. |
| **Keep running miners on through daylight gaps** | Uses paid power to sustain running miners between local sunrise and sunset. Starts still need enough solar or a paid mining window. Set your location and maximum gap duration. |
| **Daily paid electricity limit** | One shared allowance for both paid permissions. Select **No daily limit** for unlimited spending during permitted periods. |
| **Weekend plan** | Supplies separate mining hours, paid hours, Quiet Hours, and rate windows on Saturday and Sunday. The new day takes effect at local midnight. |

On Overview, **Operating schedule → Daily hours** shows the active day’s mining,
paid-start, quiet, and rate windows. With daylight gap coverage enabled, the
amber ring spans estimated sunrise to sunset. The local estimates are visible
even with **Daily hours** collapsed. Continuous daylight fills the ring, and
continuous darkness leaves it unlit. The schedule shows a status message while
estimates are unavailable. Gap coverage follows mining hours and Quiet Hours.
When the controller has a valid clock, the pointer shows its reported local
time. Read the rings clockwise from midnight, working from the outer ring
inward in the order listed. **What happens next** explains the next decision
point.

Begin with **Scheduled mining**, **Surplus only**, or **Solar with gap
coverage**, then review the populated settings. Paid permissions can overlap.
When they do, the paid mining window takes precedence and energy is counted
once. Under **Electricity pricing**, the default rate applies outside up to two
non-overlapping rate windows for each day group. Set the correct installation
time zone for schedules, rates, and daily budget resets.
[Operating rules and allowance reserves](operating-policy.md)

### Connect an energy source

Open **Settings → Energy sources** to connect a solar production source or a
whole-site grid meter. For a grid meter, compare the signed reading in the
setup form with known import and export before verifying direction.
[Energy setup and controls](integration-setup.md#verify-grid-flow-and-set-an-ess-target)

Open **Settings → Equipment & plan → Operating plan → Power settings** to set
the grid export reserve, the solar reserve for other loads, and the restart,
shedding, and preset restore settings.

**Energy → Power for mining** shows the source, miner draw, available power
for starts, and estimated paid draw. Production estimates reserve power for
other loads. A verified grid meter attributes site import to miners up to
their total draw. Other loads can contribute to that import, so use the chart
as an operating estimate.

### Choose how each miner uses presets

Open **Equipment → Power mode → Managed presets** on a compatible miner.
Authorize it with the miner's dashboard password if you want automatic
changes. Use **Evaluate presets** to collect a qualifying comparison.

- **Leave my preset as set** keeps the miner on its chosen preset.
- **Adapt to available power** can lower draw when solar falls short and
  restore the higher preset when sustained headroom returns. During an active
  daylight gap, **Energy** compares the remaining allowance at current draw
  with time until sunset and can show the estimate with a proposed lower
  preset. The gap duration still caps a continuous dip.
- **Use my measured efficiency preset** uses the qualifying preset that
  consumed less energy per unit of work.

Preset inspection and control depend on the model, firmware, available
readings, and current operating state. [Miner capability details](../integrations/miners/README.md)

## Understand a decision or recover from a problem

| If you see… | Look here |
| --- | --- |
| A miner is waiting to start | Read the outlet’s proposed action or policy decision on Overview. If draw is unknown, use the supervised test or enter a startup value. If Preview is active, review the proposed action before enabling control. |
| An unexpected stop or cost increase | In **Activity → Power & decisions**, pick a miner and interval. Compare paid energy and cost with the relay, preset, daylight gap, and source readings. **Daily budget** shows spending relative to the allowance. |
| An electricity meter gap | Open **Activity → Electricity accounting**. Resuming from fresh readings keeps recorded spending and marks the gap. Clearing accounting resets the balance and saved daily reports. |
| An unavailable or stale reading | Use **Needs attention** and **What happens next** on Overview. Check live readings in **Energy**, source configuration in **Settings → Energy sources**, and the miner in **Equipment**. Check **Settings → Clock** if local time is unavailable. |
| A miner powered on without observed work | Check the miner API and **Mining path**. Optional XBT node and DATUM observations provide upstream evidence. Set priorities, temporary holds, and optional recovery in **Equipment → Coordination**. |

Pair up to eight miners. The equipment list shows paired units, and the
pairing form offers an empty slot when one is available.

Activity keeps up to 48 hours of five-minute readings, seven days of
15-minute summaries, and 30 days of hourly summaries. Summaries preserve
average, minimum and maximum power, reading counts, energy and estimated
cost. Select an interval to inspect finer history when it is still available.
In **Today → All equipment**, the paid total updates from the same
meter readings as Overview. The chart and energy total update as intervals
close. An equipment filter shows the selected equipment’s recorded intervals.
Daily evidence remains available for seven days. Relay and preset changes
are stored with original timestamps in a separate event log.

Use **Settings → Transfer** to export version 4 or review a version 3 or 4
configuration. Import applies the plan and all equipment slots together and
opens Preview. Use the **Activity** support report for sharing diagnostics.
[Transfer contents and privacy](configuration-transfer.md)
