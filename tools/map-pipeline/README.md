# Moki Map Pipeline

Konvertiert Geofabrik OSM PBF-Daten für **BW + RP + Hessen** in ein
binäres `moki.map` Format, optimiert für ESP32-S3 + 540×960 E-Ink.

## Pipeline

```
Geofabrik PBFs  →  osmium tags-filter  →  GeoJSON  →  Python (simplify + binary)  →  moki.map
   ~1.1 GB           ~50 MB                ~30 MB                                     ~3 MB
```

## Was wird behalten

- **Flüsse** (waterway=river, riverbank): Rhein, Neckar, Main, Mosel, Lahn, Nahe, …
- **Autobahnen** (highway=motorway, motorway_link)
- **Bundesstraßen** (highway=primary, trunk)
- **Bahnstrecken** (railway=rail, ohne S-Bahn-Subnetze)
- **Städte** (place=city|town, population > 5000)
- **Wichtige Plätze** (manuell kuratierte Liste)

## Nicht im Output

- Wohnstraßen, Feldwege, Radwege
- Gebäude
- POIs außer kuratierten
- Wasserflächen (Seen → späteres Update)
- Höhenlinien

## Format

Custom binary `moki.map` (siehe `format.md`):
```
HEADER (32B):
  magic           4B   "MOKI"
  version         1B   1
  bbox_lat_min    4B   float32
  bbox_lat_max    4B   float32
  bbox_lon_min    4B   float32
  bbox_lon_max    4B   float32
  num_lines       4B
  num_points      4B   (städte/labels)
  reserved        3B

LINE (variable):
  type            1B   1=river, 2=motorway, 3=primary, 4=rail, 5=border
  zoom_min        1B   0=immer sichtbar, 1=zoom>=region, 2=zoom>=city
  num_pts         2B
  points[num_pts]:
    lat_q14       2B   uint16, quantized to bbox
    lon_q14       2B   uint16

POINT (variable):
  type            1B   1=city, 2=town, 3=village, 4=poi
  zoom_min        1B
  lat_q14         2B
  lon_q14         2B
  pop_log10       1B   log10(population), 0-9
  name_len        1B
  name            UTF-8, no trailing zero
```

Lat/Lon werden auf 16-bit quantisiert relativ zur Bbox — bei einer
Süddeutschland-Bbox von ~500km ergibt das ~7m Präzision, völlig
ausreichend für Moki-Display-Auflösung.

## Build

```bash
./build.sh
```

Schritte:
1. Lädt PBFs (wenn nicht da)
2. Extrahiert relevante Tags via `osmium tags-filter`
3. Konvertiert nach GeoJSON
4. Python-Skript: simplify (Douglas-Peucker, zoom-abhängig) + Binary-Encode
5. Output: `output/moki.map`
6. Kopiert nach `firmware/data/moki.map` für `pio run -t uploadfs`
