#!/usr/bin/env python3
"""
Konvertiert die osmium-gefilterten GeoJSONSeq-Dateien nach `moki.map` —
ein kompaktes binäres Format für den ESP32-S3-Map-Renderer.

Eingabe:  filtered/{baden-wuerttemberg,rheinland-pfalz,hessen}.geojsonl
Ausgabe:  output/moki.map

Schritte:
  1. Alle Features einlesen, Bbox berechnen
  2. Features klassifizieren (river/motorway/primary/rail/border + city/town/village)
  3. Pro Feature: Douglas-Peucker mit zoom-abhängiger Toleranz
  4. Lat/Lon nach uint16 quantisieren (relativ zur Bbox)
  5. Binär schreiben

Format-Details: siehe README.md, Section "Format".
"""

import json
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
FILTERED = ROOT / "filtered"
OUTPUT = ROOT / "output"
OUTPUT.mkdir(parents=True, exist_ok=True)

# Feature-Type-IDs (im Binary-Format)
T_RIVER     = 1
T_MOTORWAY  = 2
T_PRIMARY   = 3
T_RAIL      = 4
T_BORDER    = 5
T_SECONDARY = 6
T_TERTIARY  = 7

P_CITY      = 1
P_TOWN      = 2
P_VILLAGE   = 3
P_POI       = 4

# Zoom-Stufen (im Renderer)
Z_COUNTRY   = 0   # zeigt Süddeutschland-Übersicht
Z_REGION    = 1   # Rhein-Neckar-Maßstab
Z_CITY      = 2   # Heidelberg-Detail


def classify_line(props):
    """Returns (type_id, zoom_min) oder None wenn skip."""
    hw = props.get("highway", "")
    wt = props.get("waterway", "")
    rw = props.get("railway", "")

    if hw in ("motorway", "motorway_link"):
        return (T_MOTORWAY, Z_COUNTRY)
    if hw in ("trunk", "trunk_link"):
        return (T_MOTORWAY, Z_REGION)   # treat as motorway-light
    if hw == "primary":
        return (T_PRIMARY, Z_REGION)
    if hw == "secondary":
        return (T_SECONDARY, Z_CITY)
    if hw == "tertiary":
        return (T_TERTIARY, Z_CITY)
    if wt in ("river", "riverbank", "canal"):
        # Rhein, Neckar, Main schon im Country-Zoom; kleinere Flüsse erst
        # ab Region-Zoom. Heuristik: name in big-rivers-Whitelist
        name = (props.get("name") or "").lower()
        if name in ("rhein", "neckar", "main", "donau", "mosel", "lahn", "nahe"):
            return (T_RIVER, Z_COUNTRY)
        return (T_RIVER, Z_REGION)
    if rw == "rail":
        return (T_RAIL, Z_REGION)
    return None


def classify_point(props):
    """Returns (type_id, zoom_min, pop_log10) oder None."""
    place = props.get("place", "")
    pop_str = props.get("population") or "0"
    try:
        pop = int(pop_str.replace(",", "").replace(".", "").split()[0])
    except (ValueError, IndexError):
        pop = 0

    import math
    pop_log = min(9, max(0, int(math.log10(pop)) if pop > 0 else 0))

    if place == "city":
        return (P_CITY, Z_COUNTRY, pop_log)
    if place == "town":
        # größere Städte (>20k) auf Country-Zoom
        return (P_TOWN, Z_COUNTRY if pop >= 20000 else Z_REGION, pop_log)
    if place == "village":
        return (P_VILLAGE, Z_CITY, pop_log)
    return None


def douglas_peucker(points, epsilon):
    """Vereinfacht polyline. points = [(lat, lon), ...]."""
    if len(points) < 3:
        return points

    def perp_distance(pt, line_start, line_end):
        x0, y0 = pt
        x1, y1 = line_start
        x2, y2 = line_end
        if x1 == x2 and y1 == y2:
            return ((x0 - x1) ** 2 + (y0 - y1) ** 2) ** 0.5
        num = abs((y2 - y1) * x0 - (x2 - x1) * y0 + x2 * y1 - y2 * x1)
        denom = ((y2 - y1) ** 2 + (x2 - x1) ** 2) ** 0.5
        return num / denom if denom > 0 else 0

    # Iterativ statt rekursiv (Python-Recursion-Limit)
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        s, e = stack.pop()
        if e <= s + 1:
            continue
        max_d = 0
        max_i = s
        for i in range(s + 1, e):
            d = perp_distance(points[i], points[s], points[e])
            if d > max_d:
                max_d = d
                max_i = i
        if max_d > epsilon:
            keep[max_i] = True
            stack.append((s, max_i))
            stack.append((max_i, e))
    return [points[i] for i, k in enumerate(keep) if k]


# Vereinfachungs-Toleranz pro zoom_min (in Grad — bei 50°N: 0.001° ≈ 70m)
SIMPLIFY = {
    Z_COUNTRY: 0.005,    # ~350m, country zoom kann grob sein
    Z_REGION:  0.0008,   # ~60m
    Z_CITY:    0.0001,   # ~7m
}


def load_features():
    """Liest alle 3 GeoJSONSeq-Dateien und klassifiziert."""
    lines = []   # (type_id, zoom_min, [(lat,lon), ...])
    points = [] # (type_id, zoom_min, lat, lon, pop_log, name)
    for region in ("baden-wuerttemberg", "rheinland-pfalz", "hessen"):
        f = FILTERED / f"{region}.geojsonl"
        if not f.exists():
            print(f"WARN: missing {f}", file=sys.stderr)
            continue
        print(f"  reading {f.name}")
        with f.open() as fh:
            for raw in fh:
                raw = raw.strip()
                if not raw:
                    continue
                try:
                    feat = json.loads(raw)
                except json.JSONDecodeError:
                    continue
                geom = feat.get("geometry") or {}
                gtype = geom.get("type")
                props = feat.get("properties") or {}
                if gtype == "LineString":
                    cls = classify_line(props)
                    if not cls:
                        continue
                    type_id, zoom_min = cls
                    coords = geom.get("coordinates") or []
                    pts = [(c[1], c[0]) for c in coords]   # geojson is [lon,lat]
                    if len(pts) < 2:
                        continue
                    lines.append((type_id, zoom_min, pts))
                elif gtype == "Point":
                    cls = classify_point(props)
                    if not cls:
                        continue
                    type_id, zoom_min, pop_log = cls
                    name = (props.get("name") or "").strip()
                    if not name:
                        continue
                    coords = geom.get("coordinates") or [0, 0]
                    points.append((type_id, zoom_min, coords[1], coords[0], pop_log, name))
    return lines, points


def main():
    print("→ loading filtered geojson")
    lines, points = load_features()
    print(f"  loaded: {len(lines)} lines, {len(points)} points")

    # Bbox berechnen
    all_pts = []
    for _, _, pts in lines:
        all_pts.extend(pts)
    for _, _, lat, lon, _, _ in points:
        all_pts.append((lat, lon))
    if not all_pts:
        print("  no features — aborting")
        sys.exit(1)
    lat_min = min(p[0] for p in all_pts)
    lat_max = max(p[0] for p in all_pts)
    lon_min = min(p[1] for p in all_pts)
    lon_max = max(p[1] for p in all_pts)
    print(f"  bbox: lat {lat_min:.4f}..{lat_max:.4f}, lon {lon_min:.4f}..{lon_max:.4f}")

    # Vereinfachen
    print("→ douglas-peucker simplification")
    simplified = []
    pre = post = 0
    for type_id, zoom_min, pts in lines:
        eps = SIMPLIFY[zoom_min]
        out = douglas_peucker(pts, eps)
        pre += len(pts)
        post += len(out)
        simplified.append((type_id, zoom_min, out))
    print(f"  pts: {pre} → {post}  ({100*post/pre:.1f}%)")

    # Quantisierung-Helfer
    LAT_RANGE = lat_max - lat_min
    LON_RANGE = lon_max - lon_min

    def q_lat(lat):
        return max(0, min(65535, int((lat - lat_min) / LAT_RANGE * 65535)))

    def q_lon(lon):
        return max(0, min(65535, int((lon - lon_min) / LON_RANGE * 65535)))

    # Binary Encoding
    print("→ binary encode")
    out_path = OUTPUT / "moki.map"
    with out_path.open("wb") as fh:
        # Header (32B)
        fh.write(b"MOKI")                                # 4
        fh.write(struct.pack("<B", 1))                   # 1 version
        fh.write(struct.pack("<ffff", lat_min, lat_max, lon_min, lon_max))  # 16
        fh.write(struct.pack("<II", len(simplified), len(points)))          # 8
        fh.write(b"\x00\x00\x00")                        # 3 reserved
        # Total: 32 bytes

        # Lines
        for type_id, zoom_min, pts in simplified:
            n = len(pts)
            if n > 65535:
                pts = pts[:65535]
                n = 65535
            fh.write(struct.pack("<BBH", type_id, zoom_min, n))
            for lat, lon in pts:
                fh.write(struct.pack("<HH", q_lat(lat), q_lon(lon)))

        # Points
        for type_id, zoom_min, lat, lon, pop_log, name in points:
            name_bytes = name.encode("utf-8")[:255]
            fh.write(struct.pack("<BBHHBB",
                                 type_id, zoom_min,
                                 q_lat(lat), q_lon(lon),
                                 pop_log,
                                 len(name_bytes)))
            fh.write(name_bytes)

    sz = out_path.stat().st_size
    print(f"✓ wrote {out_path} — {sz} bytes ({sz/1024:.1f} KB)")


if __name__ == "__main__":
    main()
