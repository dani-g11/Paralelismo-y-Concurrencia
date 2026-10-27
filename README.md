# eafitOS: Compresor Huffman Concurrente integrado al Editor

**Segundo Examen Parcial de Sistemas Operativos** (SO2026B, Universidad EAFIT)
**Tema:** Concurrencia, hilos, sincronización y condiciones de carrera
**Alternativa elegida:** 1 - Compresor de Archivos Huffman Concurrente Integrado al Editor

**Integrantes:** Tomás Vera y Daniel Giraldo

---

## Visión general

`eafitOS` es un shell educativo en C que muestra las llamadas al sistema de Linux
que ejecuta cada comando. En el Primer Parcial le agregamos un editor de texto
por líneas (lista enlazada). En este Segundo Parcial le agregamos un módulo de
**compresión y descompresión con el algoritmo de Huffman**, paralelizado con
**OpenMP** (con autorización del profesor como alternativa a pthread/mutex).

Ese módulo se puede usar de dos formas:

- **Desde el shell**, con el comando `huffman`.
- **Desde el editor**, con los comandos `c` y `x`. Ahí la tarea corre en
  **segundo plano**, el prompt muestra el **progreso**, y se bloquean las
  ediciones del archivo mientras Huffman trabaja.

Toda la entrada y salida a disco usa syscalls de bajo nivel (`open`, `read`,
`write`, `lseek`, `stat`, `close`).

## Compilar y ejecutar (Linux / WSL)

```bash
make clean
make          # compila con -fopenmp y lanza ./eafitOS
```

Para elegir el número de hilos: `OMP_NUM_THREADS=4 ./eafitOS`

## Uso

**Shell**

```
huffman comp notas.txt              # -> notas.huf
huffman desc notas.huf              # -> notas.desc
huffman desc notas.huf copia.txt    # -> copia.txt (nombre de salida opcional)
help huffman
```

**Editor** (`editor notas.txt`)

| Comando | Descripción |
|---|---|
| `o`, `a`, `p`, `d`, `i`, `s`, `h`, `q` | Comandos del editor del Primer Parcial |
| `c [archivo]` | Comprime en segundo plano (sin argumento: el archivo abierto) |
| `x [archivo.huf]` | Descomprime en segundo plano (sin argumento: `<abierto>.huf`) |

Mientras hay una tarea activa, el prompt cambia a `editor [huffman 60%]>`.
Los comandos `a`, `d`, `i`, `o`, `c` y `x` quedan bloqueados.
Los comandos `p`, `s`, `h` y `q` siguen disponibles.

## Concurrencia (resumen)

| Fase | Cómo | Directivas OpenMP |
|---|---|---|
| Conteo de frecuencias | Cada hilo cuenta un bloque en su histograma local; luego se suman de forma secuencial | `parallel`, `single`, `for schedule(static)` |
| Árbol y códigos | Secuencial (cada fusión depende de la anterior) | — |
| Codificación | Cada bloque se codifica en paralelo en su propio búfer | `parallel for` |
| Ensamblado | El hilo principal escribe los bloques en orden 0, 1, 2, … | — |
| Segundo plano | Equipo de 2 hilos: uno atiende el editor y el otro ejecuta la tarea | `parallel`, `single`, `task`, `taskwait` |
| Progreso / estado | Variables compartidas entre el hilo del editor y el hilo trabajador | `atomic read/write` |

La descompresión es secuencial porque los códigos tienen longitud variable.
Al descomprimir se valida la cabecera: número mágico, que no haya frecuencias
en 0 ni símbolos repetidos y que la suma de frecuencias sea igual al tamaño
original.

## Archivos principales

| Archivo | Contenido |
|---|---|
| `main.c` | REPL del shell, tabla de comandos y `help` |
| `shell.h` | Prototipos y tipos de Huffman |
| `cat_huffman.c` | Algoritmo de Huffman, OpenMP, comando `huffman` y tarea en segundo plano |
| `cat_editor.c` | Editor con lista enlazada, comandos `c`/`x` y bloqueo de ediciones |
| `OPENMP_HUFFMAN.txt` | Explicación detallada de cada pragma y de cada decisión |
| `test_huffman.sh` | Pruebas automáticas (`bash test_huffman.sh`) |
| `bitacora_huffman.txt` | Registro de cada compresión y descompresión (fecha, tamaños, ahorro) |

## Verificación

`test_huffman.sh` cubre lo siguiente:

- Comprime y descomprime texto, un archivo vacío, un archivo de un solo
  símbolo y un binario aleatorio, y comprueba con `cmp` y `md5sum` que los
  archivos queden idénticos.
- Rechaza archivos `.huf` corruptos.
- Prueba el editor con un archivo grande: la tarea en segundo plano y el
  bloqueo de ediciones.

Resultado en WSL Ubuntu: compila sin warnings y todas las pruebas pasan.
