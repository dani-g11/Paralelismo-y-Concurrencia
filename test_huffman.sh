#!/bin/bash
# ====================================================================
# Script de pruebas del módulo Huffman (proyecto Shell SO2026B)
#   1) Compila con make.
#   2) Comprime y descomprime varios archivos desde el shell y verifica
#      con cmp + md5sum que el resultado sea IDÉNTICO al original.
#   3) Prueba que se rechacen archivos .huf corruptos.
#   4) Prueba el editor: Huffman en segundo plano, progreso y bloqueo
#      de comandos que modifican el archivo mientras Huffman trabaja.
# Uso (en Linux/WSL):   bash test_huffman.sh
# Todo se crea dentro de la carpeta pruebas_huffman/
# ====================================================================

cd "$(dirname "$0")"

echo "========================================"
echo " 1) Compilando el proyecto con make"
echo "========================================"
# El Makefile lanza el shell al terminar; le mandamos 'exit' para que se cierre.
echo "exit" | make > /dev/null || { echo "ERROR DE COMPILACION"; exit 1; }
echo "Compilado OK"

rm -rf pruebas_huffman
mkdir pruebas_huffman
cd pruebas_huffman
SHELL_BIN=../eafitOS

# Archivos de prueba
cp ../main.c codigo.c                          # texto normal
: > vacio.txt                                  # archivo vacío
printf 'aaaaaaaaaa' > uno.txt                  # un solo símbolo repetido
head -c 200000 /dev/urandom > binario.bin      # binario aleatorio

echo ""
echo "========================================"
echo " 2) Compresión / descompresión desde el shell"
echo "========================================"
for f in codigo.c vacio.txt uno.txt binario.bin; do
    base="${f%.*}"                             # nombre sin extensión
    printf "huffman comp $f\nhuffman desc $base.huf\nexit\n" | $SHELL_BIN > /dev/null 2>&1
    if cmp -s "$f" "$base.desc"; then
        echo "OK     $f -> $base.huf -> $base.desc"
    else
        echo "FALLO  $f"
    fi
done
md5sum codigo.c codigo.desc

# Tercer argumento opcional: nombre de salida
printf "huffman desc codigo.huf copia.c\nexit\n" | $SHELL_BIN > /dev/null 2>&1
cmp -s codigo.c copia.c && echo "OK     huffman desc codigo.huf copia.c"

echo ""
echo "========================================"
echo " 3) Archivos .huf corruptos (deben dar error)"
echo "========================================"
# a) No es un .huf (número mágico incorrecto)
cp codigo.c falso.huf
# b) Tamaño original alterado (byte 4 de la cabecera) -> suma de frecuencias no coincide
cp codigo.huf suma_mala.huf
printf '\x01' | dd of=suma_mala.huf bs=1 seek=4 conv=notrunc 2>/dev/null
# c) Primera frecuencia puesta en 0 (bytes 15..18 de la cabecera)
cp codigo.huf frec_cero.huf
printf '\x00\x00\x00\x00' | dd of=frec_cero.huf bs=1 seek=15 conv=notrunc 2>/dev/null
for f in falso.huf suma_mala.huf frec_cero.huf; do
    printf "huffman desc $f\nexit\n" | $SHELL_BIN 2>&1 | grep -E "Error: (El archivo|cabecera)"
done

echo ""
echo "========================================"
echo " 4) Editor: segundo plano + bloqueo de edición"
echo "========================================"
echo "(generando archivo grande para que la tarea dure unos segundos...)"
head -c 40000000 /dev/urandom | base64 > grande.txt
(
    echo "editor notas.txt"
    echo "a hola mundo"
    echo "c grande.txt"     # lanza la tarea en segundo plano
    sleep 0.3
    echo "a otra linea"     # debe BLOQUEARSE (Huffman trabajando)
    echo "o otro.txt"       # debe BLOQUEARSE
    echo "p"                # permitido: el editor sigue respondiendo
    sleep 15                # esperar a que termine la compresión
    echo "x grande.huf"     # descomprimir en segundo plano
    sleep 0.3
    echo "d 1"              # debe BLOQUEARSE
    sleep 15
    echo "a linea despues"  # ya no hay tarea: debe funcionar
    echo "q"
    echo "exit"
) | $SHELL_BIN 2>&1 | grep -E "lanzada|No se puede|exitosa|\| |Línea agregada|huffman [0-9]+%"

cmp -s grande.txt grande.desc && echo "OK     grande.txt -> grande.huf -> grande.desc"
echo "Contenido final de notas.txt:"
cat -n notas.txt

echo ""
echo "Pruebas terminadas. Archivos en: pruebas_huffman/"
