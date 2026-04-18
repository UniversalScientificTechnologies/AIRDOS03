# AIRDOS03 (UAVDOS) - Lightweight airborne dosimeter and spectrometer for UAV applications

AIRDOS03, also referred to as UAVDOS, is a compact semiconductor-based ionizing radiation dosimeter and spectrometer designed primarily for use on [unmanned aerial vehicles (UAVs)](https://en.wikipedia.org/wiki/Unmanned_aerial_vehicle). The device was developed in cooperation between [Universal Scientific Technologies s.r.o. (UST)](https://www.ust.cz/) and [ThunderFly s.r.o.](https://www.thunderfly.cz/), and is optimized for airborne radiation research missions, environmental monitoring, and atmospheric studies.

![AIRDOS03](/doc/img/AIRDOS03.jpg)

![AIRDOS03](/doc/img/AIRDOS03_Parallel-shape_interface_board.jpg)

AIRDOS03 is sensor compatible with the [TF-ATMON measurement toolchain](https://docs.thunderfly.cz/instruments/TF-ATMON) and could be used within the Pixhawk ecosystem, enabling straightforward integration with a wide range of UAV avionics.

## Design considerations

The core idea behind AIRDOS03 integrated in TF-ATMON is to minimize radiation detector payload mass by utilize resources that are already available on the UAV platform. Power supply, data logging, telemetry infrastructure, and timing or synchronization sources can be shared with onboard avionics. This approach significantly reduces the impact of the radiation sensor on flight endurance and mission range while still enabling high‑quality scientific measurements.

## Core technology

AIRDOS03 is based on the proven UST's silicon PIN diode technology. The system measures deposited energy of ionizing radiation events and provides both dosimetric and spectrometric information over a wide dynamic range.

### Radiation measurement

The detector covers an energy range of approximately 40 keV to 80 MeV with an energy resolution of 15 ± 2 keV per channel. The effective number of energy channels is around 65,000 Spectral data are typically integrated over ten seconds, although the integration time can be configured in firmware depending on mission requirements.

### Environmental sensing

To support correction of radiation data to local atmospheric conditions, AIRDOS03 includes integrated sensors for temperature and relative humidity. The temperature range spans −40 to +80 °C with an accuracy of approximately ±0.5 °C, relative humidity is measured from 0 to 100 %RH with ±2 %RH accuracy.

## Mechanical and electrical characteristics

AIRDOS03 has compact, lightweight form factor suitable for airframe integration. The electronics measure approximately 91 × 51 × 20 mm and have a total mass of about 40 g. The device is powered from a 5 V supply and typically draws around 3 mA, making it compatible even with small UAV platforms where power and mass budgets are limited.

## Interfaces and connectivity

#### UART (TELEM) connector pinout – JST-GH (Pixhawk compatible)

Communication with AIRDOS03 is primarily handled via a UART-based TELEMETRY interface using a JST‑GH connector that follows the Pixhawk connector standard. This allows direct connection to flight controllers or onboard telemetry systems. The interface supports real‑time data streaming, and data logging either onboard the UAV or on an GCS recorder. For laboratory or ground‑based use, the UART interface can be converted to USB‑C using the [TFUSBSERIAL01](https://docs.thunderfly.cz/avionics/TFUSBSERIAL01/) adapter.

| Signal | Description                             |
| ------ | --------------------------------------- |
| +5V    | Power supply input (5 V)                |
| RX     | UART receive (data to AIRDOS03)         |
| TX     | UART transmit (data from AIRDOS03)      |
| CTS    | Clear To Send (hardware flow control)   |
| RTS    | Request To Send (hardware flow control) |
| GND    | Ground                                  |

This connector follows the Pixhawk TELEM standard and allows direct connection to flight controllers or telemetry radios.

#### Auxiliary connector pinout – JST-GH

An additional auxiliary connector provides signals for precise time synchronization, external triggering, and inter-device communication. In typical deployments, this interface is used together with the [TFGPS01 GNSS receiver](https://docs.thunderfly.cz/avionics/TFGPS01/), which supplies PPS and time-pulse signals for accurate time tagging of radiation events.


| Signal    | Description                                        |
| --------- | -------------------------------------------------- |
| TIMEPULSE | PPS / time pulse input for precise synchronization |
| EXTINT    | External interrupt input                           |
| GPIO      | General-purpose digital I/O                        |
| SDA       | I2C data line                                      |
| SCL       | I2C clock line                                     |
| TX        | Auxiliary UART transmit                            |
| RX        | Auxiliary UART receive                             |
| GND       | Ground                                             |

The auxiliary interface is primarily intended for timing and synchronization but can also be used for experimental extensions or inter-device communication.

## UAV integration

AIRDOS03 is designed as a native sensor for the [TF‑ATMON](https://docs.thunderfly.cz/instruments/TF-ATMON) system and has been validated in real airborne deployments, including integration with platforms such as the [ThunderFly TF‑G2 autogyro](https://docs.thunderfly.cz/instruments/TF-G2) or [experimental stratospheric balloon platform TF-B1](https://docs.thunderfly.cz/instruments/TF-B1). In this configuration, the sensor enables mapping of radiation intensity, supports adaptive flight strategies based on measured intensity, and allows efficient utilization of limited flight time.

## Typical use cases

Typical applications of AIRDOS03 include UAV‑based atmospheric radiation surveys, detection of enhanced radiation fields in storm or convective environments, and mapping of radiation gradients near ground‑based or airborne sources. The system is also well suited for scientific support of space weather studies, aviation research, and high‑altitude dosimetry experiments.

## Software and firmware

AIRDOS03 uses modular, open-source firmware that can be adapted for specific scientific missions. Output formats are suitable for post-processing.

## Availability

AIRDOS03 can be obtained both via [ThunderFly s.r.o.](https://www.thunderfly.cz/contact-us.html) or [Universal Scientific Technologies s.r.o.](https://www.ust.cz/about/), which provides sales, integration support, and customer assistance for airborne applications. For special configurations or larger quantities, please contact us with your project requirements.

Further documentation:

* [ThunderFly AIRDOS03 documentation](https://docs.thunderfly.cz/avionics/AIRDOS03/)
* [UST ionising radiation sensors overview](https://www.ust.cz/UST-dosimeters/)

