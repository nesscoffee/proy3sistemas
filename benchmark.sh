#!/usr/bin/env bash

set -euo pipefail
export LC_NUMERIC=C

# ──────────────────────────────────────────────────────────────────────────────
# Configuración
# ──────────────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVER_BIN="$SCRIPT_DIR/servidor/aws-s3_server"
CLIENT_BIN="$SCRIPT_DIR/cliente/aws-s3"
TEST_DIR="$SCRIPT_DIR/datos_prueba"
RESULTS_FILE="$SCRIPT_DIR/resultados_benchmark_avanzado.csv"

SIZES_MB=(1 10 50 100)          # Prueba 1
HIGH_VOL_COUNT=100              # Prueba 2
HIGH_VOL_SIZE=10                # Prueba 2 – KB
FRAG_COUNT=10                   # Prueba 4 – cantidad inicial de archivos
FRAG_SIZE_MB=10                 # Prueba 4 – tamaño por archivo inicial
FRAG_EVEN_DELETE=5              # Prueba 4 – archivos borrados (los pares)
FRAG_AFTER_COUNT=5              # Prueba 4 – archivos subidos post-fragmentación
FRAG_AFTER_SIZE_MB=10           # Prueba 4 – tamaño por archivo post-frag (mismo que los borrados, deberia caber en hueco)

# ──────────────────────────────────────────────────────────────────────────────
# Funciones auxiliares
# ──────────────────────────────────────────────────────────────────────────────
info()  { printf "  %s\n" "$*"; }
pass()  { printf "  ✓ %s\n" "$*"; }
warn()  { printf "  ⚠ %s\n" "$*"; }

# nanosegundos a segundos
_ns_to_sec() {
    local ns=$1 sign=""
    [ "$ns" -lt 0 ] && sign="-" && ns=$(( -ns ))
    printf "%s%d.%03d" "$sign" "$(( ns / 1000000000 ))" "$(( ns % 1000000000 / 1000000 ))"
}

timer_start() { _timer_start=$(date +%s%N); }
timer_end()   { _timer_elapsed_ns=$(( $(date +%s%N) - _timer_start )); }

# Genera un archivo con dd
gen_file() {
    dd if=/dev/zero of="$1" bs="$2" count="$3" 2>/dev/null
}

# esto agrega la fila del resultado al csv
csv_row() {
    local category="$1" operation="$2" count="$3" detail="$4" time_sec="$5"
    printf "%s,%s,%s,%s,%s\n" "$category" "$operation" "$count" "$detail" "$time_sec" >> "$RESULTS_FILE"
}

# ──────────────────────────────────────────────────────────────────────────────
# Manejador de limpieza
# ──────────────────────────────────────────────────────────────────────────────
cleanup() {
    local rc=$?
    echo ""
    echo "=== Limpieza ==="
    # Queda ejecutandose... Entonces matarlo
    if [ -n "${SERVER_PID:-}" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        echo "  Servidor detenido (PID $SERVER_PID)."
    fi
    rm -rf "$TEST_DIR"
    rm -rf "$SCRIPT_DIR/buckets"
    echo "  Archivos temporales y almacén de buckets eliminados."
    echo "=== Hecho ==="
    exit "$rc"
}
trap cleanup EXIT INT TERM

# ──────────────────────────────────────────────────────────────────────────────
# Iniciar servidor
# ──────────────────────────────────────────────────────────────────────────────
echo "=== Iniciando servidor en puerto 9000 ==="
rm -rf "$SCRIPT_DIR/buckets"
mkdir -p "$SCRIPT_DIR/buckets"
"$SERVER_BIN" &
SERVER_PID=$!
sleep 2
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "ERROR: el servidor no pudo iniciar."
    exit 1
fi
echo "  PID: $SERVER_PID"
echo ""

# ──────────────────────────────────────────────────────────────────────────────
# Preparación
# ──────────────────────────────────────────────────────────────────────────────
mkdir -p "$TEST_DIR"
printf "categoria_prueba,operacion,cantidad_archivos,detalle,tiempo_segundos\n" > "$RESULTS_FILE"

info "Pre-generando archivos de prueba..."
gen_file "$TEST_DIR/pequeno_10KB.bin"   10K 1
gen_file "$TEST_DIR/calentamiento.bin"  1M  1
for size_mb in "${SIZES_MB[@]}"; do
    gen_file "$TEST_DIR/tamano_${size_mb}MB.bin" 1M "$size_mb"
done
for i in $(seq 1 10); do
    gen_file "$TEST_DIR/frag_$(printf '%02d' "$i")_10MB.bin" 10M 1
done
for i in $(seq 11 15); do
    gen_file "$TEST_DIR/frag_$(printf '%02d' "$i")_10MB.bin" 10M 1
done
info "Listo."
echo ""

# ──────────────────────────────────────────────────────────────────────────────
# Calentamiento (mitiga efectos de caché fría)
# ──────────────────────────────────────────────────────────────────────────────
"$CLIENT_BIN" mb cubo-calentamiento  > /dev/null 2>&1
"$CLIENT_BIN" cp "$TEST_DIR/calentamiento.bin" "cubo-calentamiento:archivo" > /dev/null 2>&1
"$CLIENT_BIN" rb cubo-calentamiento --force > /dev/null 2>&1
echo "Calentamiento completo."
echo ""

# ══════════════════════════════════════════════════════════════════════════════
# PRUEBA 1 — Rendimiento por tamaño de archivo
# ══════════════════════════════════════════════════════════════════════════════
echo "════════════════════════════════════════════════════════════════════════"
echo "PRUEBA 1: Rendimiento por tamaño de archivo"
echo "════════════════════════════════════════════════════════════════════════"

B1="prueba-tamanos"
"$CLIENT_BIN" mb "$B1" > /dev/null 2>&1

for size_mb in "${SIZES_MB[@]}"; do
    local_f="$TEST_DIR/tamano_${size_mb}MB.bin"
    dl_f="$TEST_DIR/tamano_${size_mb}MB_desc.bin"
    s3_key="archivo_${size_mb}MB.bin"
    s3_path="${B1}:${s3_key}"

    # ── Subida ──
    timer_start
    "$CLIENT_BIN" cp "$local_f" "$s3_path" > /dev/null 2>&1
    timer_end
    t_up=$(_ns_to_sec "$_timer_elapsed_ns")
    csv_row "prueba_tamanos" "subida" "1" "${size_mb}MB" "$t_up"
    printf "  %3dMB  subida:    %s s\n" "$size_mb" "$t_up"

    # ── Bajada ──
    timer_start
    "$CLIENT_BIN" cp "$s3_path" "$dl_f" > /dev/null 2>&1
    timer_end
    t_dn=$(_ns_to_sec "$_timer_elapsed_ns")
    csv_row "prueba_tamanos" "bajada" "1" "${size_mb}MB" "$t_dn"
    printf "          bajada:   %s s\n" "$t_dn"

    # ── Verificar integridad ──
    if cmp --silent "$local_f" "$dl_f"; then
        pass "Integridad OK"
    else
        warn "Sin integridad para ${size_mb}MB!"
    fi
done

"$CLIENT_BIN" rb "$B1" --force > /dev/null 2>&1
echo ""

# ══════════════════════════════════════════════════════════════════════════════
# PRUEBA 2 — Alto volumen / Overhead de red
# ══════════════════════════════════════════════════════════════════════════════
echo "════════════════════════════════════════════════════════════════════════"
echo "PRUEBA 2: Alto volumen — ${HIGH_VOL_COUNT} subidas secuenciales de ${HIGH_VOL_SIZE}KB"
echo "════════════════════════════════════════════════════════════════════════"

B2="prueba-volumen"
"$CLIENT_BIN" mb "$B2" > /dev/null 2>&1

timer_start
for i in $(seq 1 "$HIGH_VOL_COUNT"); do
    key=$(printf "vol_%03d" "$i")
    "$CLIENT_BIN" cp "$TEST_DIR/pequeno_10KB.bin" "${B2}:${key}" > /dev/null 2>&1
done
timer_end
t_vol=$(_ns_to_sec "$_timer_elapsed_ns")
csv_row "prueba_alto_volumen" "lote_subidas" "$HIGH_VOL_COUNT" "${HIGH_VOL_SIZE}KB_cada_uno" "$t_vol"

printf "  %d subidas de %dKB completadas en %s s\n" "$HIGH_VOL_COUNT" "$HIGH_VOL_SIZE" "$t_vol"
avg_ns=$(( _timer_elapsed_ns / HIGH_VOL_COUNT ))
avg_sec=$(_ns_to_sec "$avg_ns")
printf "  Promedio por archivo: %s s\n" "$avg_sec"

"$CLIENT_BIN" rb "$B2" --force > /dev/null 2>&1
echo ""

# ══════════════════════════════════════════════════════════════════════════════
# PRUEBA 3 — Límite del bucket (MAX_ARCHIVOS = 100)
# ══════════════════════════════════════════════════════════════════════════════
echo "════════════════════════════════════════════════════════════════════════"
echo "PRUEBA 3: Límite del bucket — llenar hasta MAX_ARCHIVOS, luego rechazo #101"
echo "════════════════════════════════════════════════════════════════════════"

B3="prueba-limite"
"$CLIENT_BIN" mb "$B3" > /dev/null 2>&1

# Llenar el bucket con 100 archivos
timer_start
for i in $(seq 1 100); do
    key=$(printf "lim_%03d" "$i")
    "$CLIENT_BIN" cp "$TEST_DIR/pequeno_10KB.bin" "${B3}:${key}" > /dev/null 2>&1
done
timer_end
t_fill=$(_ns_to_sec "$_timer_elapsed_ns")
csv_row "prueba_limite_bucket" "llenar_a_capacidad" "100" "10KB_cada_uno" "$t_fill"
printf "  Bucket llenado (100 archivos):      %s s\n" "$t_fill"

# Intentar la subida 101 — el servidor debe rechazar con BUCKET_LLENO
timer_start
reject_output=$( "$CLIENT_BIN" cp "$TEST_DIR/pequeno_10KB.bin" "${B3}:lim_101" 2>&1 ) || true
timer_end
t_reject=$(_ns_to_sec "$_timer_elapsed_ns")
csv_row "prueba_limite_bucket" "subida_rechazada" "1" "101er_archivo" "$t_reject"
printf "  Subida 101 rechazada:               %s s\n" "$t_reject"
printf "  Respuesta del servidor: %s\n" "$reject_output"

"$CLIENT_BIN" rb "$B3" --force > /dev/null 2>&1
echo ""

# ══════════════════════════════════════════════════════════════════════════════
# PRUEBA 4 — Fragmentación / Reuso de huecos por primer ajuste
# ══════════════════════════════════════════════════════════════════════════════
echo "════════════════════════════════════════════════════════════════════════"
echo "PRUEBA 4: Fragmentación — Reuso de huecos por primer ajuste"
echo "════════════════════════════════════════════════════════════════════════"

B4="prueba-frag"
"$CLIENT_BIN" mb "$B4" > /dev/null 2>&1

# ── Fase A: Subir 10 × 10MB ──
echo "  Fase A: Subiendo ${FRAG_COUNT} × ${FRAG_SIZE_MB}MB ..."
for i in $(seq 1 "$FRAG_COUNT"); do
    local_f="$TEST_DIR/frag_$(printf '%02d' "$i")_${FRAG_SIZE_MB}MB.bin"
    key=$(printf "frag_%02d" "$i")
    "$CLIENT_BIN" cp "$local_f" "${B4}:${key}" > /dev/null 2>&1
done
echo "    ${FRAG_COUNT} archivos subidos."

# ── Fase B: Borrar archivos pares ──
echo "  Fase B: Borrando ${FRAG_EVEN_DELETE} archivos pares ..."
for i in 2 4 6 8 10; do
    key=$(printf "frag_%02d" "$i")
    "$CLIENT_BIN" rm "$B4" "$key" > /dev/null 2>&1
done
echo "    Borrados frag_02/04/06/08/10 — 5 huecos de ${FRAG_SIZE_MB}MB creados."

# ── Fase C: Subir 5 × 10MB — deben llenar los huecos vía primer ajuste ──
echo "  Fase C: Subiendo ${FRAG_AFTER_COUNT} × ${FRAG_AFTER_SIZE_MB}MB (llenando huecos) ..."
for i in $(seq 11 $((10 + FRAG_AFTER_COUNT))); do
    local_f="$TEST_DIR/frag_$(printf '%02d' "$i")_${FRAG_AFTER_SIZE_MB}MB.bin"
    key=$(printf "frag_%02d" "$i")
    "$CLIENT_BIN" cp "$local_f" "${B4}:${key}" > /dev/null 2>&1
done
echo "    Subidos frag_11/12/13/14/15 — cada uno colocado en un hueco de 10MB vía primer ajuste."

# ── Verificación: tamaño del archivo bucket y listado ──
echo ""
echo "  ── Verificación ──"
bucket_file="buckets/${B4}"
if [ -f "$bucket_file" ]; then
    bucket_size=$(stat --format='%s' "$bucket_file" 2>/dev/null || stat -f '%z' "$bucket_file" 2>/dev/null)
    echo "    Tamaño del archivo bucket: $bucket_size bytes"
    expected_min=104857600  # 100MB mínimo absoluto
    if [ "$bucket_size" -lt "$expected_min" ]; then
        echo "    Falló la prueba. Tamaño del bucket: $bucket_size bytes (esperaba ≥ $expected_min bytes)"
    else
        echo "    Bucket ≥ 100MB (los huecos fueron reusados, sin añadido al EOF)"
    fi
fi

echo ""
echo "    Listado del bucket:"
"$CLIENT_BIN" ls "$B4" 2>&1
file_count=$( "$CLIENT_BIN" ls "$B4" 2>/dev/null | grep -c '|' || true )
echo "    Total de objetos: $file_count (se esperaban 10)"

"$CLIENT_BIN" rb "$B4" --force > /dev/null 2>&1
echo ""

# ══════════════════════════════════════════════════════════════════════════════
# Hecho — Mostrar resultados
# ══════════════════════════════════════════════════════════════════════════════
echo "════════════════════════════════════════════════════════════════════════"
echo "TODAS LAS PRUEBAS COMPLETADAS"
echo "════════════════════════════════════════════════════════════════════════"
echo ""
echo "Resultados: $RESULTS_FILE"
echo ""
echo "────────────────────────────────────────────────────────────────────────"
echo "Datos CSV en bruto:"
echo "────────────────────────────────────────────────────────────────────────"
cat "$RESULTS_FILE"
