#!/bin/bash
# Filter Geofabrik PBFs auf nur Moki-relevante Features.
# Eingabe: pbf/{baden-wuerttemberg,rheinland-pfalz,hessen}.osm.pbf
# Ausgabe: filtered/<region>.geojson  (oder GeoJSONSeq, je nach Variante)

set -euo pipefail

cd "$(dirname "$0")"
mkdir -p filtered

REGIONS=(baden-wuerttemberg rheinland-pfalz hessen)

for region in "${REGIONS[@]}"; do
  in="pbf/${region}.osm.pbf"
  filtered="filtered/${region}-filtered.osm.pbf"
  out="filtered/${region}.geojsonl"

  if [[ ! -f "$in" ]]; then
    echo "MISSING: $in"
    exit 1
  fi

  echo "→ filter ${region}"
  # ways:    Autobahnen, Bundesstr., Stadthauptstraßen, Flüsse, Bahn
  # nodes:   Städte mit place=
  osmium tags-filter "$in" \
    w/highway=motorway,motorway_link,trunk,trunk_link,primary,secondary,tertiary \
    w/waterway=river,riverbank,canal \
    w/railway=rail \
    n/place=city,town,village \
    --overwrite -o "$filtered"

  # GeoJSON-seq export: ein Feature pro Zeile, einfach in Python zu lesen
  echo "→ export ${region} → geojsonl"
  osmium export "$filtered" \
    --geometry-types=linestring,point \
    --output-format=geojsonseq \
    --overwrite -o "$out"

  echo "  size: $(du -h "$out" | cut -f1)"
done

echo "✓ filter.sh done"
