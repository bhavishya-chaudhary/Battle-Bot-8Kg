# Battle Bot — 8 kg Vertical Spinner Combat Robot


An **8 kg vertical spinner combat robot** designed and built as a team project for RoboWars.

The robot combines a custom SolidWorks-designed aluminium chassis, UHMWPE and stainless-steel protection, a chain-driven vertical spinner, high-current power electronics, an ESP32-based control system, and multiple independent safety mechanisms.

The project followed a **prototype → validation → final integration → competition** workflow, bringing together mechanical design, manufacturing, electronics, embedded programming, control systems, and system integration.

---

## Project Overview

The robot was designed around two primary systems:

- **Differential / tank drivetrain** for independent left and right control and zero-radius turning
- **High-speed vertical spinner** for the weapon system

The ESP32 sits between the RC receiver and the actuators, providing additional control, configuration, and safety functionality instead of directly passing receiver signals to the motor controllers.

### System Architecture

```text
                     FlySky FS-i6S
                        Transmitter
                            │
                            ▼
                    FlySky FS-i10AB
                         Receiver
                            │
                            ▼
                     ┌─────────────┐
                     │    ESP32    │
                     │             │
                     │ RC Input    │
                     │ Mixing      │
                     │ Curves      │
                     │ Weapon Ctrl │
                     │ Arming      │
                     │ Failsafes   │
                     │ Web UI      │
                     │ OTA         │
                     └──────┬──────┘
                            │
                 ┌──────────┴──────────┐
                 │                     │
                 ▼                     ▼
          BTS7960 Drivers           80 A ESC
                 │                     │
                 ▼                     ▼
          Drive Motors           Weapon Motor
                 │                     │
                 ▼                     ▼
              Wheels            Chain + Sprocket
                                       │
                                       ▼
                               Vertical Spinner
```

---

## Key Specifications

| Parameter | Specification |
|---|---|
| Robot class | 8 kg combat robot |
| Configuration | Vertical spinner |
| Drive | Differential / tank steering |
| Drive motors | 24 V geared DC motors |
| Drive motor speed | ~468 RPM |
| Drive motor torque | ~72.6 N·cm |
| Drive motor drivers | BTS7960 |
| Wheels | 100 mm heavy-duty wheels |
| Weapon motor | D3548 BLDC |
| Weapon motor KV | 790 KV |
| Weapon ESC | 80 A, 6S|
| Weapon transmission | Chain + sprocket |
| Controller | ESP32 Wroom |
| Transmitter | FlySky FS-i6S |
| Receiver | FlySky FS-i10AB |
| Battery | 2 × 3S 4200 mAh 35C LiPo in series |
| Battery configuration | 6S, 4200 mAh |
| Low-voltage supply | XL4051 5 A buck converter |
| High-current wiring | AWG 10 silicone wire |
| Main connectors | XT60 |
| Chassis | Aluminium |
| Side protection | 15 mm UHMWPE + stainless-steel reinforcement |
| Control interface | RC + ESP32 Web UI |
| Firmware update | OTA |
| Competition | RoboWars |

> **Note:** Some values above are component specifications or design values rather than independently measured final-system performance.

---

## Mechanical Design

### Chassis

The main chassis was designed in **SolidWorks**, with the mechanical and electrical systems considered together during the packaging stage.

The layout was organized around the two major systems:

```text
        FRONT
          │
          ▼
   ┌───────────────┐
   │    WEAPON     │
   │   ASSEMBLY    │
   ├───────────────┤
   │               │
   │   ELECTRONICS │
   │    BATTERY    │
   │      ESC      │
   │               │
   ├───────────────┤
   │   DRIVETRAIN  │
   │   DRIVE MOTORS│
   │    & WHEELS   │
   └───────────────┘
          │
         REAR
```

The final structure used:

- Aluminium chassis plates
- 15 mm UHMWPE side protection
- Stainless-steel reinforcement and protection plates
- Mechanical fasteners
- Integrated mounts for the drivetrain, weapon, battery, and electronics

The CAD assembly was used to plan component placement before fabrication.

---

## Weapon System

The bot uses a **vertical spinner weapon** driven by a BLDC motor through a chain-and-sprocket transmission.


The chain-driven arrangement allowed the motor and weapon assembly to be packaged within the chassis while providing flexibility in the mechanical layout and working without putting much tension on motor shaft.

### Weapon RPM


The weapon RPM was about ***15000 RPM**.


### Attacker CAD Mass Properties

The SolidWorks calculated porperties of the Vertical Spinner are as follows:

| Property | CAD Value |
|---|---:|
| Mass | 557.24 g |
| Volume | 70,975.33 mm³ |
| Density | 0.007858 g/mm³ |

The model's reported principal moments included approximately:

- Px: 187,361.64 g·mm²
- Py: 351,575.39 g·mm²
- Pz: 386,648.41 g·mm²

> **Important:** This CAD mass represents the referenced attacking spinner model and **not the complete rotating weapon assembly**. The referenced model information did not include the shaft, sprocket, and motor.

---

## Drivetrain

The robot uses **differential / tank steering**, allowing the left and right drive systems to be controlled independently.

This provides:

- Forward and reverse motion
- Differential steering
- Zero-radius turning
- Independent left and right control

### Drive Motors

The drivetrain uses:

- 24 V geared DC motors
- Approximately 468 RPM
- Approximately 72.6 N·cm torque

### Motor Drivers

Each drive side is controlled using **BTS7960 high-current motor drivers**.

---

## Wheels & Traction

The robot uses **100 mm heavy-duty wheels**.

To improve traction, cricket-bat grip rubber was cut and applied to the drive wheels.

This was a simple, readily available solution for increasing wheel grip without requiring a specialized wheel compound.

The drive system also used approximately **8 mm ID motor-wheel coupling arrangements** for anti-slip motor-to-wheel connection.

---

## Electronics & Control

One of the main technical aspects of the project was the decision to place an **ESP32 control layer between the RC receiver and the actuators**.

This allowed additional control and safety functionality to be implemented in software.

---

## ESP32 Control System

The ESP32 handled:

- RC signal processing
- Channel mixing
- Tank steering
- Ploynomial throttle & steering responses
- Weapon control
- Weapon power modes
- Arming and disarming
- Failsafe handling
- Diagnostics
- Web UI
- OTA firmware updates

---

### Non-linear Throttle & Steering

The control system used **ploynomial throttle and steering curves** with tunable index rather than a simple linear mapping.


This was particularly useful for combat operation, where precise positioning and rapid movement can both be important.

---

## Weapon Power Modes

The weapon control system provided three levels of weapon output using the same physical potentiometer.

Modes are changed by a 3 position switch on Transmitter.

### Mode 1 — Controlled

The potentiometer command was scaled to approximately **50% of the full power**.

### Mode 2 — Higher Power

The potentiometer command was scaled to approximately **90% of the full power**.

### Mode 3 — Full Power

A full-power override bypassed the potentiometer scaling and commanded approximately **100% power**.

```text
          Weapon Power
               │
      ┌────────┼────────┐
      ▼        ▼        ▼
   ~50%      ~90%      100%
 Controlled  Higher    Full
   Mode      Power     Power
```

The intention was to provide controllable weapon operation during normal manoeuvring while retaining higher-power modes when required.

---

## Independent Arming

The drivetrain and weapon were designed with **independent arming and disarming control**.

This allowed combinations such as:

- Drive armed / weapon disarmed
- Drive disarmed / weapon armed
- Both disarmed
- Both armed

Independent arming provided an additional layer of operational safety during testing.

---

## Safety Architecture

Because the weapon is a high-energy rotating system, safety was treated as a system-level requirement rather than relying on a single software failsafe.

The design incorporated multiple layers:

### Physical Kill Switch

A physically accessible hardware kill switch provided a direct means of shutting down the robot.

### Radio-loss Failsafe

Loss of the expected RC signal initiated a safe-state response intended to stop the drivetrain and weapon.

### Watchdog

An ESP32 watchdog was used to detect firmware execution problems and help prevent the system from remaining in an unsafe commanded state.

### Brownout Handling

ESP32 brownout and reset conditions were also considered as part of the safety architecture.

### Emergency / Failsafe Command

The control system included a user-triggered emergency/failsafe mechanism.

## Web UI

The ESP32 provided a live **Web UI** for configuration, tuning, and diagnostics.

It was used for:

- Throttle curve tuning
- Steering curve tuning
- Control parameter adjustment
- Diagnostics
- Testing
- Configuration
- Monitoring

This reduced the need to repeatedly modify firmware source code and re-upload it simply to adjust control parameters.

---

## OTA Firmware Updates

The ESP32 firmware supported **Over-The-Air (OTA) updates**.

This allowed firmware iterations to be performed without always physically connecting the robot to a computer.

---

## Power System

The robot used:

**2 × 3S 4200 mAh 35C LiPo batteries connected in series**

Resulting in:

```text
Configuration: 6S
Capacity:      4200 mAh
Nominal:       22.2 V
Full charge:   25.2 V
```

The theoretical continuous current capability based on the stated 35C rating is:

```text
4.2 Ah × 35 C = 147 A
```

> This is a theoretical value based on the battery rating and should not be interpreted as a measured system current capability.

---

## Power Electronics

The electrical system included:

- 80 A ESC for the BLDC weapon motor
- BTS7960 motor drivers
- XL4051 5 A buck converter
- Decoupling capacitors
- TVS transient protection
- AWG 10 silicone high-current wiring
- XT60 connectors
- 24 V cooling fan

The design also considered grounding, electrical noise, and transient protection because of the high-current motors and rapidly changing loads.

---

## Cooling

A **24 V cooling fan** was incorporated for thermal management of the electronics and power system.

This was particularly relevant to:

- High-current motor drivers
- BLDC ESC
- Drive motors
- Control electronics
- High-current wiring

---

## Manufacturing

Fabrication was split between outsourced manufacturing and in-house work.

### Outsourced

- Laser cutting of aluminium chassis plates
- Wire cutting of the vertical spinner / attacker

### College Laboratory Work

The remaining work was carried out by the team in the college labs, including:

- Manufacturing (Cutting, Drilling, Grinding, Soldering, etc)
- Mechanical assembly
- Electronics assembly
- Wiring
- Component mounting
- System integration
- Testing
- Final adjustments

---

## Development Process

The robot was developed incrementally rather than being assembled as a final system immediately.

### 1. CAD & System Design

The chassis and component arrangement were developed in SolidWorks.

Mechanical and electrical packaging were considered together so that the complete system could be assembled within the available space.

### 2. Simplified Prototype

A simpler prototype was developed before the final combat chassis.

The prototype was used to validate:

- Electronics
- ESP32 firmware
- RC communication
- Motor control
- Weapon control
- Channel mixing
- Safety logic
- Failsafes

The prototype used a **direct flexible weapon coupling**, unlike the final chain-and-sprocket transmission.

### 3. Control & Firmware Validation

The ESP32 control architecture was developed and tested, including:

- Channel processing
- Mixing
- Control curves
- Weapon modes
- Arming
- Failsafes
- Web UI
- OTA updates

### 4. Final Integration

After validating the electronics and control system, the systems were integrated into the final chassis.

The final robot incorporated:

- Aluminium chassis
- UHMWPE protection
- Stainless-steel reinforcement
- Chain-driven weapon
- Final weapon support
- Integrated electronics
- Final wiring
- Complete combat configuration

## Competition

The completed robot was taken to **RoboWars** and competed in an actual combat environment.

The robot did not win its match in Round 2.

The critical failure occurred in the **weapon transmission**, where the sprocket broke under combat loading.

The failure ultimately stopped the weapon system and contributed to the loss.

---

## Failure Analysis & Engineering Lesson

The competition provided a useful real-world validation of the complete system.

The robot had been tested under normal operating conditions, but combat introduced substantially more severe transient loading.

The sprocket failure demonstrated an important engineering principle:

> **A component that works during normal operation may still fail when subjected to the transient loads of its real application.**

The failure highlighted the importance of:

- Impact-load analysis
- Component selection
- Transmission sizing
- Stress concentration
- Shock loading
- Fatigue considerations
- Testing under representative conditions

Rather than treating the competition failure simply as a loss, it became one of the most valuable engineering outcomes of the project.

---

## Key Engineering Features

| Area | Implementation |
|---|---|
| Mechanical | SolidWorks-designed aluminium combat chassis + UHMWPE/stainless protection + chain-driven vertical spinner |
| Drivetrain | Differential/tank steering + geared DC motors + BTS7960 high-current drivers |
| Electrical | 6S LiPo power architecture + BLDC weapon + high-current wiring + transient protection |
| Embedded | ESP32-based control layer between RC receiver and actuators |
| Control | Channel mixing + non-linear throttle/steering response + configurable weapon power |
| Safety | Physical kill switch + radio failsafe + watchdog + brownout handling + emergency failsafe + independent arming |
| Software | Live Web UI + parameter tuning + diagnostics + OTA firmware updates |
| Development | Prototype-first validation → final system integration → competition |
| Manufacturing | Outsourced precision cutting + in-house laboratory assembly and integration |
| Validation | RoboWars competition + post-failure engineering analysis |

---

## My Contribution

This was a **team project under Team Spartans**, with different members contributing to the overall system.

My primary responsibilities covered a large part of the technical design and integration.

### Mechanical

- Designed the main robot chassis in SolidWorks
- Designed the internal component arrangement
- Designed the weapon mounting and support structure
- Selected mechanical components and architecture
- Worked on drivetrain, weapon transmission, protection, and packaging decisions

### Electronics

- Designed the electronics architecture
- Selected major electrical components
- Worked on power distribution and control architecture
- Integrated the ESC, motor drivers, ESP32, receiver, battery system, and buck converter
- Worked on high-current wiring and connections

### Embedded & Control

- Developed the ESP32-based control architecture
- Implemented channel mixing
- Implemented non-linear throttle and steering response
- Implemented weapon control and power modes
- Implemented arming and disarming logic
- Implemented failsafe and safety functionality
- Implemented Web UI functionality
- Implemented OTA firmware updating

### System Integration

- Integrated mechanical, electrical, and software systems
- Worked through prototype validation
- Supported final assembly and testing
- Helped take the system from CAD and individual subsystems to a competition-ready robot

> AI tools were used as development assistance during firmware work, while the system requirements, architecture, control behaviour, and engineering decisions were defined and directed by the project team.

---

## Technology & Components

### Mechanical / CAD

- SolidWorks
- Aluminium chassis
- UHMWPE
- Stainless steel
- Chain and sprocket transmission
- Bearings
- 100 mm wheels

### Electronics

- ESP32
- BTS7960
- 80 A ESC
- D3548 790 KV BLDC motor
- 24 V geared DC motors
- XL4051 buck converter
- TVS protection
- XT60 connectors

### Control

- FlySky FS-i6S
- FlySky FS-i10AB
- Custom ESP32 firmware
- Web UI
- OTA updates

### Power

- 2 × 3S 4200 mAh 35C LiPo
- 6S configuration
- AWG 10 silicone wiring

### Manufacturing

- Laser cutting
- Wire cutting
- College laboratory fabrication and assembly

---

## Project Takeaways

This project provided hands-on experience across the complete engineering lifecycle:

```text
Concept
  ↓
CAD
  ↓
Component Selection
  ↓
Manufacturing
  ↓
Electronics
  ↓
Embedded Firmware
  ↓
Prototype
  ↓
Testing
  ↓
Integration
  ↓
Competition
  ↓
Failure Analysis
```

The most valuable aspect of the project was not any individual component. It was the requirement to make **mechanical, electrical, embedded, and control systems work together inside a machine exposed to vibration, impacts, high currents, and rapidly changing loads**.

The project demonstrated practical experience in:

**Mechanical Design + Manufacturing + Electronics + Embedded Systems + Control + Safety + System Integration**

---

## Status

**Completed — Competed at RoboWars**

The robot reached competition, where the weapon transmission sprocket failed under combat loading. The failure provided direct feedback on the limitations of the mechanical transmission design and highlighted the importance of designing and validating components for transient impact loads, not only steady-state operation.

---

## License

This project is released under the [MIT License](LICENSE).
