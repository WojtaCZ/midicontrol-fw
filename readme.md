# Special MIDI Messages

## SysEx zprávy pro ovládání displeje

Tento dokument popisuje **strukturu MIDI SysEx zpráv** používaných pro ovládání displeje.
Všechny zprávy používají **stejnou hlavičku**, liší se pouze **cílem zprávy** a **datovou částí**.

---

## Obecná struktura SysEx zprávy

Každý rámec má následující strukturu (bez standardních MIDI `F0` / `F7`, které se řeší mimo tento protokol):

```
[HLAVIČKA] [ZAŘÍZENÍ] [CÍL] [DATA...]
```

| Pole     | Velikost | Popis                                    |
| -------- | -------- | ---------------------------------------- |
| Hlavička | 2 B      | Identifikace protokolu         |
| Zařízení | 1 B      | Typ cílového zařízení                    |
| Cíl      | 1 B      | Určuje, která část displeje se nastavuje |
| Data     | N B      | Data odpovídající danému cíli            |

---

## Fixní hlavička

Hlavička je **pevná a povinná** pro všechny zprávy.

| Byte | Hodnota |
| ---- | ------- |
| 0    | `0x4D`  |
| 1    | `0x43`  |


---

## Typ zařízení

| Hodnota | Význam  |
| ------: | ------- |
|  `0x01` | Displej |

Ostatní hodnoty jsou **rezervovány**.

---

## Cíl zprávy

Cíl určuje, **která část displeje se aktualizuje** a tím pádem i strukturu datové části.

| Hodnota | Cíl          |
| ------: | ------------ |
|  `0x00` | Celý displej |
|  `0x01` | Číslo písně  |
|  `0x02` | Číslo sloky  |
|  `0x03` | Písmeno      |
|  `0x04` | LED          |

---

# Kódování hodnot

## Čísla (píseň, sloka)

* Každý znak = **1 nibble (4 bity)**
* `0x0–0x9` → číslice
* `0xF` → mezera

### Číslo písně

* Rozsah: `0–1999`
* Velikost: **2 byty (4 znaky)**

| Byty        | Zobrazení |
| ----------- | --------- |
| `0x12 0x34` | `1234`    |
| `0x0F 0x23` | `0 23`    |

---

### Číslo sloky

* Rozsah: `0–99`
* Velikost: **1 byte (2 znaky)**

| Byte   | Zobrazení |
| ------ | --------- |
| `0x09` | `09`      |
| `0xF5` | ` 5`      |

---

## Písmeno

Kódováno jedním nibblem (nižší 4 bity):

| Hodnota | Zobrazení |
| ------: | --------- |
|   `0xA` | A         |
|   `0xB` | B         |
|   `0xC` | C         |
|   `0xD` | D         |
|   `0xF` | zhasnuto    |

---

## LED

| Hodnota | Význam      |
| ------: | ----------- |
|   `0x1` | Červená     |
|   `0x2` | Zelená      |
|   `0x3` | Modrá       |
|   `0x4` | Žlutá       |
|   `0xF` | zhasnuto |


# Jednotlivé typy rámců

## Nastavení celého displeje (`Cíl = 0x00`)

Používá se pro **kompletní aktualizaci** (např. po zapnutí).

### Struktura rámce

```
0x4D 0x43 | 0x01 | 0x00 | [SONG_H] [SONG_L] | [VERSE] | [LETTER] | [LED]
```

| Pole        | Velikost | Popis             |
| ----------- | -------- | ----------------- |
| Číslo písně | 2 B      | 4 nibble (0–1999) |
| Číslo sloky | 1 B      | 2 nibble (0–99)   |
| Písmeno     | 1 B      | 1 nibble          |
| LED         | 1 B      | Barva / vypnuto   |

### Příklad

Zobrazí:

* píseň `1234`
* sloku `05`
* písmeno `B`
* LED zelená

```
0x4D 0x43 0x01 0x00  0x12 0x34  0x05  0x0B  0x02
```

---

## Nastavení pouze čísla písně (`Cíl = 0x01`)

### Struktura

```
0x4D 0x43 | 0x01 | 0x01 | [SONG_H] [SONG_L]
```

### Příklad

Zobrazí `0078`:

```
0x4D 0x43 0x01 0x01  0x00 0x78
```

---

## Nastavení pouze čísla sloky (`Cíl = 0x02`)

### Struktura

```
0x4D 0x43 | 0x01 | 0x02 | [VERSE]
```

### Příklad

Zobrazí ` 5`:

```
0x4D 0x43 0x01 0x02  0xF5
```

---

## Nastavení pouze písmene (`Cíl = 0x03`)

### Struktura

```
0x4D 0x43 | 0x01 | 0x03 | [LETTER]
```

### Příklad

Zobrazí `C`:

```
0x4D 0x43 0x01 0x03  0x0C
```

---

## Nastavení LED (`Cíl = 0x04`)

### Struktura

```
0x4D 0x43 | 0x01 | 0x04 | [LED]
```

### Příklad

LED žlutá:

```
0x4D 0x43 0x01 0x04  0x04
```

---

