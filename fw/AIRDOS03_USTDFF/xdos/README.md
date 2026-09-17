# Kontrola formátu výstupu (xdos-check)

Firmware se po buildu spustí v simulaci ATmega1284P (simavr) proti modelu desky v
[board.yaml](board.yaml) a zkontroluje se, že jeho výstup pořád odpovídá
[UST Dosimeters File Format](https://docs.dos.ust.cz/xdos_format).

**Lokálně kontrola jen varuje a build nikdy nezastaví.** Blokovat bude až CI.

Firmware se kvůli kontrole nijak neupravuje a simuluje se přesně ten ELF, který PlatformIO
právě sestavil.

## Co je tady

| Soubor | K čemu je |
|---|---|
| [board.yaml](board.yaml) | co je na které I2C adrese a na kterém pinu |
| [scenarios/basic.yaml](scenarios/basic.yaml) | co simulace do firmwaru pošle (události, senzory, GNSS) |
| [checker.txt](checker.txt) | pinovaná verze kontroly |
| [hook.py](hook.py) | napojení na PlatformIO (build i upload) |
| `golden/` | referenční záznam z posledního release, zatím prázdné |
| [../xdos_check.ini](../xdos_check.ini) | extra config, který hook zapíná |

Vlastní kontroly, schéma formátu a modely součástek jsou v balíčku `ust_format_checker`
v [DOSPORTAL](https://github.com/UniversalScientificTechnologies/DOSPORTAL), aby byly
společné pro všechna xDOS zařízení.

## Nastavení

Jediná podmínka je simavr, zbytek si hook udělá sám:

```bash
sudo apt install simavr libsimavr-dev libelf-dev gcc
```

Při prvním buildu si hook podle [checker.txt](checker.txt) vytvoří vlastní venv v
`~/.cache/xdos-check/` a nainstaluje do něj pinovanou verzi (jednorázově, řádově sekundy).
Pak už se jen spouští a na síť nesahá. Do prostředí PlatformIO se nic neinstaluje.

Spuštění:

```bash
pio run -c xdos_check.ini -e TFUNIPAYLOAD01_uart            # kontrola po buildu
pio run -c xdos_check.ini -e TFUNIPAYLOAD01_uart -t upload  # i před uploadem
XDOS_CHECK=0 pio run -c xdos_check.ini -e TFUNIPAYLOAD01_uart   # kontrola vypnutá
```

Bez simavr (nebo když se instalace nepovede) hook napíše jednu řádku a build normálně doběhne.

### Vývoj samotné kontroly

Když pracuješ na checkeru, dá se pin obejít checkoutem DOSPORTAL:

```bash
export XDOS_CHECKER_PATH=~/cesta/k/DOSPORTAL/backend
```

Tahle varianta navíc kontroluje výstup parserem z DOSPORTAL, což pinovaná instalace zatím
neumí (`packages.parsing` není samostatný balíček) a napíše to do výstupu jako info.

## Kolik to zdrží

Naměřeno na tomhle projektu:

| Situace | Čas celého `pio run` |
|---|---|
| první build (vytvoření venv) | 7,1 s |
| build beze změny binárky | 0,7 s (kontrola neběží) |
| build po změně, která mění binárku | 1,5 s |
| `-t upload` bez rebuildu, změněná binárka | 1,4 s |

Kontrola tedy stojí zhruba 0,8 s a spouští se jen tehdy, když se ELF opravdu změnil (hlídá se
otisk souboru). Změna, po které vznikne bajtově stejná binárka — třeba přidaný komentář — ji
nespustí. Dva měřicí bloky se stihnou odsimulovat pod sekundu, protože se čas ve firmwaru
posouvá dopředu a nečeká se na reálných deset sekund.

## Když kontrola něco najde

Výstup je ve stylu Rustu: co přesně nesedí, proč to vadí a co s tím. Tři úrovně:

- **error** - formát je rozbitý, v CI to zastaví merge,
- **warning** - stojí za pozornost, například když DOSPORTAL řádek tiše zahazuje,
- **info** - konstatování, třeba nová zpráva nebo pole přidané na konec.

Záměrná změna formátu se potvrdí uložením nového referenčního záznamu:

```bash
~/.cache/xdos-check/*/bin/xdos-check-firmware .pio/build/TFUNIPAYLOAD01_uart/firmware.elf \
    --board xdos/board.yaml --scenario xdos/scenarios/basic.yaml \
    --golden xdos/golden/basic.txt --accept
```

Nový záznam se pak commitne a v PR je změna formátu vidět jako čitelný diff.

## Vlastní scénář

Scénáře jsou v [scenarios/](scenarios). Hodnoty smějí být náhodné v mezích součástky, seed se
odvozuje od názvu scénáře, takže běhy zůstávají reprodukovatelné:

```yaml
name: high_rate
stop_blocks: 2
sensors:
  sht31: {temp_c: rand(-40, 85), humidity: rand(0, 100)}
events:
  at: 1.2
  channels: [0, 12, 40, 63, 64, 100, 500, 1023]
gnss: {fix_at: 1.0, unix: 1789560000}
```
