/**
 * ============================================================================
 * Proyecto: Compresor y Descompresor de Archivos con Algoritmo de Huffman
 * Asignatura: Sistemas Operativos (4to Semestre)
 * 
 * GUÍA TEÓRICA SOBRE LLAMADAS AL SISTEMA (SYSTEM CALLS):
 * ----------------------------------------------------------------------------
 * 1. ¿Qué es una llamada al sistema (syscall)?
 *    Es la interfaz formal y segura mediante la cual un programa que se ejecuta
 *    en espacio de usuario (User Space / Modo No Privilegiado) solicita servicios
 *    directamente al núcleo del sistema operativo (Kernel Space / Modo Privilegiado).
 * 
 * 2. Transición de Modo (Context Switch de Modo):
 *    Cuando invocamos open(), read(), write() o close():
 *    a) Los argumentos se colocan en registros específicos de la CPU.
 *    b) Se ejecuta una instrucción de trampa/interrupción por software (syscall / int 0x80).
 *    c) La CPU cambia al nivel de privilegio 0 (Modo Núcleo).
 *    d) El despachador de syscalls del Kernel ejecuta la rutina correspondiente
 *       interactuando con los controladores de disco (drivers) y el sistema de archivos.
 *    e) El resultado se devuelve y la CPU retorna al Modo Usuario.
 * 
 * 3. ¿Qué es un Descriptor de Archivo (File Descriptor - fd)?
 *    Es un número entero no negativo (int fd) que sirve como índice dentro de
 *    la Tabla de Descriptores de Archivos del Proceso, administrada por el SO.
 *    - 0: Entrada estándar (STDIN_FILENO)
 *    - 1: Salida estándar (STDOUT_FILENO)
 *    - 2: Error estándar (STDERR_FILENO)
 *    - 3+: Asignados a archivos abiertos por el usuario.
 * 
 * 4. Búferes de Entrada/Salida a nivel de Bloque:
 *    Cada llamada al sistema tiene un costo de tiempo por cambio de contexto.
 *    Para optimizar el rendimiento, agrupamos las lecturas y escrituras en
 *    búferes de 4096 bytes (4 KB), coincidiendo con el tamaño de página y bloque
 *    estándar del sistema de archivos en los sistemas operativos modernos.
 * ============================================================================
 */

#include "shell.h"      /* Tipos de Huffman (NodoHuffman, Diccionario) y prototipos */
#include <string.h>     /* strcpy, strcmp, strlen, strrchr */
#include <time.h>       /* time, localtime, strftime (bitácora) */
#include <omp.h>        /* OpenMP: omp_get_thread_num, omp_get_num_threads, omp_get_max_threads */

// Máximo de hilos que soportamos (para dimensionar los arreglos locales por hilo)
#define MAX_HILOS 64


#define TAMANO_BLOQUE_IO 4096

/*
 * ============================================================================
 * MÓDULO AUXILIAR: Escritor de Bits con Búfer de Bloque y Syscall write()
 * ============================================================================
 */
typedef struct {
    int descriptor_archivo;                 // File Descriptor (fd) asignado por open()
    unsigned char bufer_bits;               // Byte donde se acumulan los bits (0 a 8)
    int bits_acumulados;                    // Cantidad de bits acumulados en bufer_bits
    unsigned char bufer_bloque[TAMANO_BLOQUE_IO]; // Búfer de 4 KB para emitir escrituras en bloque
    size_t bytes_en_bloque;                 // Cantidad de bytes en bufer_bloque esperando write()
} EscritorBits;

/**
 * Inicializa el escritor de bits vinculándolo al descriptor de archivo (fd).
 */
static void inicializar_escritor_bits(EscritorBits *escritor, int descriptor_archivo) {
    escritor->descriptor_archivo = descriptor_archivo;
    escritor->bufer_bits = 0;
    escritor->bits_acumulados = 0;
    escritor->bytes_en_bloque = 0;
}

/**
 * Envía el bloque acumulado de bytes a disco invocando la llamada al sistema write().
 */
static void vaciar_bloque_escritura(EscritorBits *escritor) {
    if (escritor->bytes_en_bloque > 0) {
        size_t escritos_totales = 0;
        while (escritos_totales < escritor->bytes_en_bloque) {
            // Syscall write(fd, buffer, cantidad)
            ssize_t res = write(escritor->descriptor_archivo, 
                                escritor->bufer_bloque + escritos_totales, 
                                escritor->bytes_en_bloque - escritos_totales);
            if (res < 0) {
                perror("Error en llamada al sistema write() en EscritorBits");
                return;
            }
            escritos_totales += (size_t)res;
        }
        escritor->bytes_en_bloque = 0;
    }
}

/**
 * Agrega un bit (0 o 1) al búfer de bits. Al completar 8 bits, transfiere el byte
 * al búfer de bloque. Si el bloque de 4 KB se llena, invoca write() al sistema operativo.
 */
static void escribir_bit(EscritorBits *escritor, int valor_bit) {
    // Desplazar a la izquierda e insertar el bit
    escritor->bufer_bits = (unsigned char)((escritor->bufer_bits << 1) | (valor_bit & 1));
    escritor->bits_acumulados++;

    // Al completar 8 bits (1 byte)
    if (escritor->bits_acumulados == 8) {
        escritor->bufer_bloque[escritor->bytes_en_bloque++] = escritor->bufer_bits;
        escritor->bufer_bits = 0;
        escritor->bits_acumulados = 0;

        // Si se completó una página de 4096 bytes, emitimos la llamada al sistema write()
        if (escritor->bytes_en_bloque == TAMANO_BLOQUE_IO) {
            vaciar_bloque_escritura(escritor);
        }
    }
}

/**
 * Rellena con ceros a la derecha los bits pendientes y vacía el búfer hacia el disco.
 */
static void vaciar_escritor_bits(EscritorBits *escritor) {
    if (escritor->bits_acumulados > 0) {
        escritor->bufer_bits <<= (8 - escritor->bits_acumulados);
        escritor->bufer_bloque[escritor->bytes_en_bloque++] = escritor->bufer_bits;
        escritor->bufer_bits = 0;
        escritor->bits_acumulados = 0;
    }
    vaciar_bloque_escritura(escritor);
}

/*
 * ============================================================================
 * MÓDULO AUXILIAR: Lector de Bits con Búfer de Bloque y Syscall read()
 * ============================================================================
 */
typedef struct {
    int descriptor_archivo;                 // File Descriptor (fd) para lectura
    unsigned char bufer_bloque[TAMANO_BLOQUE_IO]; // Bloque de 4 KB leído con read()
    size_t bytes_disponibles;               // Bytes devueltos en la última llamada a read()
    size_t indice_lectura;                  // Posición del byte actual dentro de bufer_bloque
    unsigned char bufer_bits;               // Byte actual del que se están extrayendo bits
    int bits_restantes;                     // Bits aún no leídos en bufer_bits (0 a 8)
} LectorBits;

static void inicializar_lector_bits(LectorBits *lector, int descriptor_archivo) {
    lector->descriptor_archivo = descriptor_archivo;
    lector->bytes_disponibles = 0;
    lector->indice_lectura = 0;
    lector->bufer_bits = 0;
    lector->bits_restantes = 0;
}

/**
 * Lee el siguiente bit de la secuencia. Cuando se agota el búfer interno,
 * invoca la llamada al sistema read() para cargar una página de 4 KB desde el disco.
 */
static int leer_bit(LectorBits *lector) {
    if (lector->bits_restantes == 0) {
        // Si ya procesamos todos los bytes del bloque de 4 KB, leemos un nuevo bloque
        if (lector->indice_lectura >= lector->bytes_disponibles) {
            // Syscall read(fd, buffer, cantidad)
            ssize_t leidos = read(lector->descriptor_archivo, lector->bufer_bloque, TAMANO_BLOQUE_IO);
            if (leidos <= 0) {
                return -1; // Fin de archivo o error de lectura
            }
            lector->bytes_disponibles = (size_t)leidos;
            lector->indice_lectura = 0;
        }

        // Extraer el siguiente byte del bloque
        lector->bufer_bits = lector->bufer_bloque[lector->indice_lectura++];
        lector->bits_restantes = 8;
    }

    // Extraer el bit más significativo (MSB a LSB)
    lector->bits_restantes--;
    int valor_bit = (lector->bufer_bits >> lector->bits_restantes) & 1;
    return valor_bit;
}

/*
 * ============================================================================
 * 1. FUNCIÓN: leer_archivo (Implementada con Llamadas al Sistema)
 * ============================================================================
 * Llamadas al Sistema Utilizadas:
 *   - open(): Abre el archivo solicitando al SO un nuevo descriptor en la tabla.
 *   - lseek(): Modifica y consulta el puntero de posición (offset) en el Kernel.
 *   - read(): Solicita al Kernel la transferencia de datos físicos a la RAM.
 *   - close(): Libera el descriptor de archivo del proceso.
 */
unsigned char* leer_archivo(const char *ruta_archivo, size_t *tamano_salida) {
    if (!ruta_archivo || !tamano_salida) {
        return NULL;
    }

    // 1. LLAMADA AL SISTEMA: open()
    // Bandera O_RDONLY: Abre el archivo exclusivamente en modo de solo lectura.
    int descriptor_archivo = open(ruta_archivo, O_RDONLY);
    if (descriptor_archivo == -1) {
        perror("Error en llamada al sistema open() al intentar leer");
        return NULL;
    }

    // 2. LLAMADA AL SISTEMA: lseek()
    // lseek(fd, 0, SEEK_END): Sitúa el offset en el último byte y retorna la posición,
    // permitiendo averiguar el tamaño exacto del archivo en bytes.
    off_t longitud_archivo = lseek(descriptor_archivo, 0, SEEK_END);
    if (longitud_archivo < 0) {
        perror("Error en llamada al sistema lseek()");
        close(descriptor_archivo);
        return NULL;
    }

    // Reposicionar el offset al inicio del archivo (SEEK_SET, 0)
    if (lseek(descriptor_archivo, 0, SEEK_SET) < 0) {
        perror("Error al reposicionar lseek()");
        close(descriptor_archivo);
        return NULL;
    }

    *tamano_salida = (size_t)longitud_archivo;

    // Caso especial: archivo vacío
    if (longitud_archivo == 0) {
        close(descriptor_archivo);
        unsigned char *bufer_vacio = (unsigned char*)malloc(1);
        return bufer_vacio;
    }

    // Reservar memoria en el Heap para almacenar el contenido completo
    unsigned char *bufer_datos = (unsigned char*)malloc((size_t)longitud_archivo);
    if (!bufer_datos) {
        fprintf(stderr, "Error: Memoria insuficiente en el Heap para cargar el archivo.\n");
        close(descriptor_archivo);
        return NULL;
    }

    // 3. LLAMADA AL SISTEMA: read()
    // En sistemas operativos POSIX, read() puede leer menos bytes de los solicitados
    // si ocurre una interrupción por señal o límites del sistema de archivos.
    // Por ello, es una buena práctica utilizar un bucle hasta completar el tamaño total.
    size_t bytes_totales_leidos = 0;
    while (bytes_totales_leidos < (size_t)longitud_archivo) {
        ssize_t bytes_leidos_iteracion = read(descriptor_archivo, 
                                             bufer_datos + bytes_totales_leidos, 
                                             (size_t)longitud_archivo - bytes_totales_leidos);
        if (bytes_leidos_iteracion < 0) {
            perror("Error en llamada al sistema read()");
            free(bufer_datos);
            close(descriptor_archivo);
            return NULL;
        }
        if (bytes_leidos_iteracion == 0) {
            break; // Fin inesperado del archivo
        }
        bytes_totales_leidos += (size_t)bytes_leidos_iteracion;
    }

    // 4. LLAMADA AL SISTEMA: close()
    // Cierra el descriptor para evitar agotamiento de descriptores de archivos en el SO.
    close(descriptor_archivo);
    return bufer_datos;
}

/*
 * ============================================================================
 * 2. FUNCIÓN: contar_frecuencias
 * ============================================================================
 */
/*
 * VERSIÓN PARALELA CON OpenMP (reducción local + suma secuencial)
 * ----------------------------------------------------------------------------
 * Problema: si todos los hilos hicieran frecuencias[token]++ sobre el MISMO
 * arreglo, dos hilos podrían leer/escribir la misma casilla a la vez
 * (condición de carrera) y se perderían conteos.
 *
 * Solución:
 *   1. Cada hilo tiene SU PROPIO arreglo de 256 contadores (frecuencias_locales[hilo]).
 *   2. Se reparte el archivo en bloques contiguos, uno por hilo (parallel for).
 *      Cada hilo solo cuenta en su arreglo local -> nadie comparte memoria de escritura.
 *   3. Al terminar todos los hilos, el hilo principal suma los arreglos locales
 *      en el arreglo global de forma SECUENCIAL. Sin hilos -> sin carreras.
 *
 * Así no necesitamos 'critical' ni 'atomic' dentro del bucle (serían muy lentos,
 * porque se ejecutarían una vez por cada byte del archivo).
 */
void contar_frecuencias(const unsigned char *datos, 
                        size_t tamano_datos, 
                        uint32_t frecuencias[MAX_SIMBOLOS]) {
    // Un histograma por hilo. 'static' para no ocupar 64 KB de la pila.
    static uint32_t frecuencias_locales[MAX_HILOS][MAX_SIMBOLOS];
    int hilos_usados = 1;

    // Pedimos como máximo MAX_HILOS hilos para no salirnos del arreglo
    int hilos_pedidos = omp_get_max_threads();
    if (hilos_pedidos > MAX_HILOS) {
        hilos_pedidos = MAX_HILOS;
    }

    // PRAGMA 1: 'parallel' crea el equipo de hilos. Todo el bloque { } lo ejecutan todos.
    // Variables declaradas DENTRO del bloque son privadas de cada hilo (id_hilo, i...).
    #pragma omp parallel num_threads(hilos_pedidos)
    {
        int id_hilo = omp_get_thread_num();   // 0, 1, 2, ... (privada)

        // PRAGMA 2: 'single' -> solo UN hilo guarda cuántos hilos hay realmente
        // (los demás esperan en la barrera implícita al final del single).
        #pragma omp single
        {
            hilos_usados = omp_get_num_threads();
        }

        // Cada hilo limpia su propio histograma (no hay conflicto: fila distinta)
        for (int s = 0; s < MAX_SIMBOLOS; s++) {
            frecuencias_locales[id_hilo][s] = 0;
        }

        // PRAGMA 3: 'for' reparte las iteraciones entre los hilos.
        // schedule(static) = el arreglo se parte en bloques (chunks) contiguos del
        // mismo tamaño: hilo 0 -> primer bloque, hilo 1 -> segundo bloque, etc.
        // Al final del 'for' hay una barrera implícita: nadie sale hasta que todos terminan.
        #pragma omp for schedule(static)
        for (size_t i = 0; i < tamano_datos; i++) {
            unsigned char token = datos[i];
            frecuencias_locales[id_hilo][token]++;   // solo escribe en SU fila
        }
    } // <- fin de la región paralela: barrera implícita, todos los hilos terminaron

    // REDUCCIÓN SECUENCIAL: solo el hilo principal suma los histogramas locales
    for (int s = 0; s < MAX_SIMBOLOS; s++) {
        frecuencias[s] = 0;
        for (int h = 0; h < hilos_usados; h++) {
            frecuencias[s] += frecuencias_locales[h][s];
        }
    }
}

/*
 * ============================================================================
 * 3. FUNCIÓN: crear_arbol_huffman   (SECUENCIAL A PROPÓSITO, sin OpenMP)
 * ============================================================================
 * Cada fusión depende del resultado de la anterior (el nodo padre nuevo entra
 * a competir como mínimo en el siguiente paso). No se puede calcular el paso
 * k+1 sin terminar el paso k. Además son como máximo 255 fusiones: es muy
 * poco trabajo, crear hilos costaría más de lo que se ganaría.
 */
static NodoHuffman* crear_nodo(unsigned char simbolo, 
                               uint32_t frecuencia, 
                               NodoHuffman *hijo_izquierdo, 
                               NodoHuffman *hijo_derecho) {
    NodoHuffman *nuevo_nodo = (NodoHuffman*)malloc(sizeof(NodoHuffman));
    if (!nuevo_nodo) {
        fprintf(stderr, "Error: No se pudo asignar memoria para un NodoHuffman.\n");
        exit(EXIT_FAILURE);
    }
    nuevo_nodo->simbolo = simbolo;
    nuevo_nodo->frecuencia = frecuencia;
    nuevo_nodo->izq = hijo_izquierdo;
    nuevo_nodo->der = hijo_derecho;
    return nuevo_nodo;
}

NodoHuffman* crear_arbol_huffman(const uint32_t frecuencias[MAX_SIMBOLOS]) {
    NodoHuffman *lista_nodos[MAX_SIMBOLOS];
    int cantidad_nodos = 0;

    for (int i = 0; i < MAX_SIMBOLOS; i++) {
        if (frecuencias[i] > 0) {
            lista_nodos[cantidad_nodos++] = crear_nodo((unsigned char)i, frecuencias[i], NULL, NULL);
        }
    }

    if (cantidad_nodos == 0) {
        return NULL;
    }

    if (cantidad_nodos == 1) {
        return crear_nodo(0, lista_nodos[0]->frecuencia, lista_nodos[0], NULL);
    }

    while (cantidad_nodos > 1) {
        int indice_min1 = 0;
        for (int i = 1; i < cantidad_nodos; i++) {
            if (lista_nodos[i]->frecuencia < lista_nodos[indice_min1]->frecuencia) {
                indice_min1 = i;
            }
        }
        NodoHuffman *primer_minimo = lista_nodos[indice_min1];

        lista_nodos[indice_min1] = lista_nodos[cantidad_nodos - 1];
        cantidad_nodos--;

        int indice_min2 = 0;
        for (int i = 1; i < cantidad_nodos; i++) {
            if (lista_nodos[i]->frecuencia < lista_nodos[indice_min2]->frecuencia) {
                indice_min2 = i;
            }
        }
        NodoHuffman *segundo_minimo = lista_nodos[indice_min2];

        NodoHuffman *nodo_padre = crear_nodo(0, 
                                             primer_minimo->frecuencia + segundo_minimo->frecuencia, 
                                             primer_minimo, 
                                             segundo_minimo);

        lista_nodos[indice_min2] = nodo_padre;
    }

    return lista_nodos[0];
}

/*
 * ============================================================================
 * 4. FUNCIÓN: generar_codigos   (SECUENCIAL A PROPÓSITO, sin OpenMP)
 * ============================================================================
 * Recorre un árbol de como máximo 511 nodos usando un único búfer
 * 'prefijo_temporal' compartido por la recursión. Paralelizarlo obligaría a
 * duplicar ese búfer por hilo y el trabajo es mínimo comparado con el archivo.
 */
static void recorrer_arbol_recursivo(NodoHuffman *nodo_actual, 
                                     char *prefijo_temporal, 
                                     int profundidad, 
                                     Diccionario *diccionario) {
    if (!nodo_actual) {
        return;
    }

    if (!nodo_actual->izq && !nodo_actual->der) {
        prefijo_temporal[profundidad] = '\0';

        if (profundidad == 0) {
            strcpy(diccionario->tabla[nodo_actual->simbolo].codigo, "0");
            diccionario->tabla[nodo_actual->simbolo].longitud = 1;
        } else {
            strcpy(diccionario->tabla[nodo_actual->simbolo].codigo, prefijo_temporal);
            diccionario->tabla[nodo_actual->simbolo].longitud = profundidad;
        }

        diccionario->presente[nodo_actual->simbolo] = true;
        return;
    }

    if (nodo_actual->izq) {
        prefijo_temporal[profundidad] = '0';
        recorrer_arbol_recursivo(nodo_actual->izq, prefijo_temporal, profundidad + 1, diccionario);
    }

    if (nodo_actual->der) {
        prefijo_temporal[profundidad] = '1';
        recorrer_arbol_recursivo(nodo_actual->der, prefijo_temporal, profundidad + 1, diccionario);
    }
}

void generar_codigos(NodoHuffman *raiz, Diccionario *diccionario) {
    for (int i = 0; i < MAX_SIMBOLOS; i++) {
        diccionario->tabla[i].codigo[0] = '\0';
        diccionario->tabla[i].longitud = 0;
        diccionario->presente[i] = false;
    }

    if (!raiz) {
        return;
    }

    char prefijo_temporal[MAX_LONGITUD_CODIGO];
    recorrer_arbol_recursivo(raiz, prefijo_temporal, 0, diccionario);
}

/*
 * ============================================================================
 * 5. FUNCIÓN: guardar_comprimido (Implementada con Llamadas al Sistema)
 * ============================================================================
 * Llamadas al Sistema Utilizadas:
 *   - open(): con banderas O_WRONLY | O_CREAT | O_TRUNC y permisos 0644.
 *   - write(): escribe directamente los encabezados binarios y el flujo de bits.
 *   - close(): confirma el vaciado de buffers del Kernel y cierra el descriptor.
 */
int guardar_comprimido(const char *ruta_salida, 
                       const unsigned char *datos_originales, 
                       size_t tamano_original, 
                       const uint32_t frecuencias[MAX_SIMBOLOS], 
                       const Diccionario *diccionario) {
    // Garantizar que la carpeta de destino exista (llamada a mkdir)
    asegurar_directorio_padre(ruta_salida);

    // 1. LLAMADA AL SISTEMA: open()
    // Banderas explicadas para estudiantes de SO:
    //   - O_WRONLY: Abrir únicamente para escritura (Write Only).
    //   - O_CREAT: Si el archivo no existe en el inodo, el Kernel lo crea.
    //   - O_TRUNC: Si el archivo ya existía, limpia su contenido truncándolo a 0 bytes.
    // Modo de Permisos (0644 en notación octal POSIX):
    //   - 6 (rw-): El usuario propietario puede leer y escribir.
    //   - 4 (r--): Los miembros del grupo solo pueden leer.
    //   - 4 (r--): Los demás usuarios solo pueden leer.
    int fd_salida = open(ruta_salida, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_salida == -1) {
        perror("Error en llamada al sistema open() al crear archivo comprimido");
        return -1;
    }

    // 2. LLAMADA AL SISTEMA: write() para metadatos de la cabecera
    // a) Escribir el número mágico (4 bytes)
    uint32_t numero_magico = NUMERO_MAGICO_HUFFMAN;
    if (write(fd_salida, &numero_magico, sizeof(uint32_t)) != sizeof(uint32_t)) {
        perror("Error en llamada al sistema write() escribiendo numero magico");
        close(fd_salida);
        return -1;
    }

    // b) Escribir el tamaño original en bytes (uint64_t = 8 bytes)
    uint64_t tamano_en_bytes = (uint64_t)tamano_original;
    if (write(fd_salida, &tamano_en_bytes, sizeof(uint64_t)) != sizeof(uint64_t)) {
        perror("Error en llamada al sistema write() escribiendo tamano original");
        close(fd_salida);
        return -1;
    }

    // c) Contar y escribir la cantidad de símbolos distintos
    uint16_t simbolos_distintos = 0;
    for (int i = 0; i < MAX_SIMBOLOS; i++) {
        if (frecuencias[i] > 0) {
            simbolos_distintos++;
        }
    }
    if (write(fd_salida, &simbolos_distintos, sizeof(uint16_t)) != sizeof(uint16_t)) {
        perror("Error en llamada al sistema write() escribiendo cantidad de simbolos");
        close(fd_salida);
        return -1;
    }

    // d) Escribir los pares de frecuencias del diccionario
    for (int i = 0; i < MAX_SIMBOLOS; i++) {
        if (frecuencias[i] > 0) {
            unsigned char simbolo = (unsigned char)i;
            write(fd_salida, &simbolo, sizeof(unsigned char));
            write(fd_salida, &frecuencias[i], sizeof(uint32_t));
        }
    }

    if (tamano_original == 0) {
        close(fd_salida);
        return 0;
    }

    // 3. CODIFICACIÓN PARALELA POR BLOQUES (OpenMP)
    // ------------------------------------------------------------------------
    // El árbol y el diccionario YA están construidos (secuencialmente) y aquí
    // solo se LEEN, así que todos los hilos pueden consultarlos sin peligro.
    //
    // Fase A (paralela): el archivo se divide en 'num_bloques' bloques. Cada hilo
    //   codifica su bloque en un búfer de bits PROPIO en memoria (bloques[b]).
    //   Ningún hilo escribe en el búfer de otro -> sin condiciones de carrera.
    //
    // Fase B (secuencial): el hilo principal recorre los bloques en orden
    //   0, 1, 2, ... y los escribe en el archivo. Así se garantiza que la salida
    //   quede en el orden correcto (requisito de "ensamblado ordenado").
    //   No se puede hacer en paralelo porque los bloques no terminan justo en
    //   un byte completo: los bits de un bloque continúan en el byte del anterior.

    int num_bloques = omp_get_max_threads();
    if (num_bloques > MAX_HILOS) {
        num_bloques = MAX_HILOS;
    }

    // Resultado de cada bloque: sus bits empaquetados y cuántos bits son
    unsigned char *bits_bloque[MAX_HILOS];
    size_t cantidad_bits_bloque[MAX_HILOS];

    // Tamaño de cada bloque redondeado hacia arriba (el último puede ser más corto)
    size_t tamano_bloque = (tamano_original + (size_t)num_bloques - 1) / (size_t)num_bloques;

    // PRAGMA 4: 'parallel for' = crear hilos + repartir las iteraciones.
    // Cada iteración 'b' es un bloque independiente; 'b' es privada de cada hilo.
    #pragma omp parallel for num_threads(num_bloques) schedule(static)
    for (int b = 0; b < num_bloques; b++) {
        // Rango [inicio, fin) de bytes del archivo que le tocan a este bloque
        size_t inicio = (size_t)b * tamano_bloque;
        size_t fin = inicio + tamano_bloque;
        if (inicio > tamano_original) inicio = tamano_original;
        if (fin > tamano_original) fin = tamano_original;

        // a) Contar cuántos bits ocupará el bloque para reservar la memoria justa
        size_t total_bits = 0;
        for (size_t i = inicio; i < fin; i++) {
            total_bits += (size_t)diccionario->tabla[datos_originales[i]].longitud;
        }

        // b) Reservar el búfer del bloque (calloc lo deja en ceros). +1 por si está vacío.
        unsigned char *bufer = (unsigned char*)calloc(total_bits / 8 + 1, 1);
        size_t posicion_bit = 0;

        // c) Codificar: por cada byte, copiar los bits de su código al búfer
        if (bufer != NULL) {
            for (size_t i = inicio; i < fin; i++) {
                const char *cadena_codigo = diccionario->tabla[datos_originales[i]].codigo;
                for (int j = 0; cadena_codigo[j] != '\0'; j++) {
                    if (cadena_codigo[j] == '1') {
                        // Encender el bit número 'posicion_bit' (de izquierda a derecha)
                        bufer[posicion_bit / 8] |= (unsigned char)(1 << (7 - (posicion_bit % 8)));
                    }
                    posicion_bit++;
                }
            }
        }

        // d) Guardar el resultado en la casilla de ESTE bloque (casilla distinta por hilo)
        bits_bloque[b] = bufer;
        cantidad_bits_bloque[b] = posicion_bit;
    } // <- barrera implícita: aquí todos los bloques ya están codificados

    // Verificar que ningún hilo se quedó sin memoria
    int hubo_error = 0;
    for (int b = 0; b < num_bloques; b++) {
        if (bits_bloque[b] == NULL) {
            hubo_error = 1;
        }
    }

    // Fase B (secuencial y ORDENADA): escribir los bloques 0, 1, 2, ... al archivo
    if (!hubo_error) {
        EscritorBits escritor;
        inicializar_escritor_bits(&escritor, fd_salida);

        for (int b = 0; b < num_bloques; b++) {
            for (size_t k = 0; k < cantidad_bits_bloque[b]; k++) {
                int valor_bit = (bits_bloque[b][k / 8] >> (7 - (k % 8))) & 1;
                escribir_bit(&escritor, valor_bit);
            }
        }

        // Vaciar los bits residuales hacia el descriptor mediante write()
        vaciar_escritor_bits(&escritor);
    }

    // Liberar los búferes de todos los bloques (free(NULL) no hace nada)
    for (int b = 0; b < num_bloques; b++) {
        free(bits_bloque[b]);
    }

    if (hubo_error) {
        fprintf(stderr, "Error: Memoria insuficiente al codificar los bloques.\n");
        close(fd_salida);
        return -1;
    }

    // 4. LLAMADA AL SISTEMA: close()
    close(fd_salida);
    return 0;
}

/*
 * ============================================================================
 * 6. FUNCIÓN: descomprimir_archivo (Implementada con Llamadas al Sistema)
 *    (SECUENCIAL A PROPÓSITO, sin OpenMP)
 * ============================================================================
 * Los códigos de Huffman tienen longitud variable y el archivo es un único
 * flujo de bits: no se sabe dónde empieza el símbolo N sin haber decodificado
 * los N-1 anteriores. Por eso un hilo no puede saltar a la mitad del archivo.
 * ----------------------------------------------------------------------------
 * Llamadas al Sistema Utilizadas:
 *   - open(): para abrir el archivo comprimido (O_RDONLY) y crear el destino (O_WRONLY | O_CREAT | O_TRUNC).
 *   - read(): lee la cabecera binaria y el flujo comprimido.
 *   - write(): escribe bloques de bytes decodificados directamente en el archivo destino.
 *   - close(): cierra ambos descriptores.
 */
int descomprimir_archivo(const char *ruta_comprimido, const char *ruta_descomprimido) {
    // 1. LLAMADA AL SISTEMA: open() para lectura del comprimido
    int fd_entrada = open(ruta_comprimido, O_RDONLY);
    if (fd_entrada == -1) {
        perror("Error en llamada al sistema open() al abrir archivo comprimido");
        return -1;
    }

    // 2. LLAMADA AL SISTEMA: read() para validar el número mágico
    uint32_t numero_magico_leido = 0;
    if (read(fd_entrada, &numero_magico_leido, sizeof(uint32_t)) != sizeof(uint32_t) || 
        numero_magico_leido != NUMERO_MAGICO_HUFFMAN) {
        fprintf(stderr, "Error: El archivo no es un archivo Huffman valido (Magic Number incorrecto).\n");
        close(fd_entrada);
        return -1;
    }

    // 3. LLAMADA AL SISTEMA: read() para leer el tamaño original esperado
    uint64_t tamano_original = 0;
    if (read(fd_entrada, &tamano_original, sizeof(uint64_t)) != sizeof(uint64_t)) {
        fprintf(stderr, "Error al leer tamano original con llamada read().\n");
        close(fd_entrada);
        return -1;
    }

    // 4. LLAMADA AL SISTEMA: read() para leer cantidad de símbolos
    uint16_t simbolos_distintos = 0;
    if (read(fd_entrada, &simbolos_distintos, sizeof(uint16_t)) != sizeof(uint16_t)) {
        fprintf(stderr, "Error al leer cantidad de simbolos con llamada read().\n");
        close(fd_entrada);
        return -1;
    }

    // 5. LLAMADA AL SISTEMA: read() para reconstruir la tabla de frecuencias
    // VALIDACIÓN: no puede haber más de 256 símbolos distintos (un byte tiene 256 valores)
    if (simbolos_distintos > MAX_SIMBOLOS) {
        fprintf(stderr, "Error: cabecera corrupta (%u simbolos, maximo %d).\n",
                (unsigned)simbolos_distintos, MAX_SIMBOLOS);
        close(fd_entrada);
        return -1;
    }

    uint32_t frecuencias[MAX_SIMBOLOS] = {0};
    uint64_t suma_frecuencias = 0;   // para comparar con tamano_original al final
    for (int i = 0; i < simbolos_distintos; i++) {
        unsigned char simbolo = 0;
        uint32_t frecuencia = 0;
        if (read(fd_entrada, &simbolo, sizeof(unsigned char)) != sizeof(unsigned char) ||
            read(fd_entrada, &frecuencia, sizeof(uint32_t)) != sizeof(uint32_t)) {
            fprintf(stderr, "Error al leer diccionario de frecuencias con llamada read().\n");
            close(fd_entrada);
            return -1;
        }

        // VALIDACIÓN 1: una frecuencia guardada nunca puede ser 0
        // (al comprimir solo se guardan los símbolos que SÍ aparecen)
        if (frecuencia == 0) {
            fprintf(stderr, "Error: cabecera corrupta (el simbolo %u tiene frecuencia 0).\n",
                    (unsigned)simbolo);
            close(fd_entrada);
            return -1;
        }

        // VALIDACIÓN 2: cada símbolo aparece UNA sola vez en la tabla.
        // Si su casilla ya tiene un valor, es porque está repetido.
        if (frecuencias[simbolo] != 0) {
            fprintf(stderr, "Error: cabecera corrupta (el simbolo %u esta repetido).\n",
                    (unsigned)simbolo);
            close(fd_entrada);
            return -1;
        }

        frecuencias[simbolo] = frecuencia;
        suma_frecuencias += frecuencia;
    }

    // VALIDACIÓN 3: la suma de todas las frecuencias es el número total de bytes
    // del archivo original. Si no coincide con el tamaño guardado, algo está mal.
    if (suma_frecuencias != tamano_original) {
        fprintf(stderr, "Error: cabecera corrupta (suma de frecuencias %lu != tamano original %lu).\n",
                (unsigned long)suma_frecuencias, (unsigned long)tamano_original);
        close(fd_entrada);
        return -1;
    }

    // Asegurar directorio destino
    asegurar_directorio_padre(ruta_descomprimido);

    // 6. LLAMADA AL SISTEMA: open() para crear el archivo restaurado
    int fd_salida = open(ruta_descomprimido, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_salida == -1) {
        perror("Error en llamada al sistema open() al crear archivo restaurado");
        close(fd_entrada);
        return -1;
    }

    // Si el archivo original estaba vacío
    if (tamano_original == 0) {
        close(fd_salida);
        close(fd_entrada);
        return 0;
    }

    // 7. Reconstruir el árbol de Huffman con las frecuencias obtenidas
    NodoHuffman *raiz = crear_arbol_huffman(frecuencias);
    if (!raiz) {
        fprintf(stderr, "Error al reconstruir el arbol de Huffman.\n");
        close(fd_salida);
        close(fd_entrada);
        return -1;
    }

    // 8. Decodificación de bits y escritura mediante búfer de bloque y llamada write()
    LectorBits lector;
    inicializar_lector_bits(&lector, fd_entrada);

    uint64_t bytes_recuperados = 0;
    NodoHuffman *nodo_actual = raiz;

    // Búfer intermedio para agrupar salidas y emitir llamadas al sistema write() óptimas
    unsigned char bufer_escritura[TAMANO_BLOQUE_IO];
    size_t bytes_en_bufer_escritura = 0;

    // Caso de un solo símbolo único repetido
    if (!raiz->izq && !raiz->der) {
        for (uint64_t i = 0; i < tamano_original; i++) {
            bufer_escritura[bytes_en_bufer_escritura++] = raiz->simbolo;
            if (bytes_en_bufer_escritura == TAMANO_BLOQUE_IO) {
                write(fd_salida, bufer_escritura, bytes_en_bufer_escritura);
                bytes_en_bufer_escritura = 0;
            }
        }
        bytes_recuperados = tamano_original;
    } else {
        while (bytes_recuperados < tamano_original) {
            int valor_bit = leer_bit(&lector);
            if (valor_bit == -1) {
                fprintf(stderr, "Advertencia: Fin prematuro de la secuencia de bits.\n");
                break;
            }

            nodo_actual = (valor_bit == 0) ? nodo_actual->izq : nodo_actual->der;

            // Al llegar a una hoja, encontramos el byte original
            if (nodo_actual && !nodo_actual->izq && !nodo_actual->der) {
                bufer_escritura[bytes_en_bufer_escritura++] = nodo_actual->simbolo;
                bytes_recuperados++;
                nodo_actual = raiz; // Reiniciar recorrido

                // Cuando el búfer alcanza 4 KB, invocar la syscall write()
                if (bytes_en_bufer_escritura == TAMANO_BLOQUE_IO) {
                    write(fd_salida, bufer_escritura, bytes_en_bufer_escritura);
                    bytes_en_bufer_escritura = 0;

                    // Avisar el progreso (de 0 a 99%) para que el editor lo muestre
                    huffman_poner_progreso((int)(bytes_recuperados * 99 / tamano_original));
                }
            }
        }
    }

    // Vaciar bytes pendientes restantes en el búfer de salida
    if (bytes_en_bufer_escritura > 0) {
        write(fd_salida, bufer_escritura, bytes_en_bufer_escritura);
    }

    // 9. LLAMADAS AL SISTEMA: close()
    close(fd_salida);
    close(fd_entrada);
    liberar_arbol(raiz);

    if (bytes_recuperados != tamano_original) {
        fprintf(stderr, "Error: Se recuperaron %lu bytes de los %lu esperados.\n",
                (unsigned long)bytes_recuperados, (unsigned long)tamano_original);
        return -1;
    }

    return 0;
}

/*
 * ============================================================================
 * 7. FUNCIÓN: liberar_arbol
 * ============================================================================
 */
void liberar_arbol(NodoHuffman *raiz) {
    if (!raiz) {
        return;
    }
    liberar_arbol(raiz->izq);
    liberar_arbol(raiz->der);
    free(raiz);
}

/*
 * ============================================================================
 * 8. FUNCIÓN: asegurar_directorio_padre (Implementada con Llamadas al Sistema)
 * ============================================================================
 * Llamadas al Sistema Utilizadas:
 *   - stat(): Consulta los inodos y metadatos del directorio sin abrirlo.
 *   - mkdir(): Crea una entrada de directorio en el inodo correspondiente con
 *              permisos 0755 (rwxr-xr-x).
 */
void asegurar_directorio_padre(const char *ruta_archivo) {
    if (!ruta_archivo) return;

    const char *ultimo_separador = strrchr(ruta_archivo, '/');
    if (!ultimo_separador) {
        return; // Ubicado en el directorio actual (.)
    }

    size_t longitud_dir = (size_t)(ultimo_separador - ruta_archivo);
    if (longitud_dir == 0) {
        return; // Ubicado en la raíz (/)
    }

    char ruta_directorio[512];
    if (longitud_dir >= sizeof(ruta_directorio)) {
        longitud_dir = sizeof(ruta_directorio) - 1;
    }
    strncpy(ruta_directorio, ruta_archivo, longitud_dir);
    ruta_directorio[longitud_dir] = '\0';

    // LLAMADA AL SISTEMA: stat() para interrogar inodos
    struct stat estado;
    if (stat(ruta_directorio, &estado) == -1) {
        // LLAMADA AL SISTEMA: mkdir(ruta, permisos)
        // Permisos 0755: rwxr-xr-x (lectura/escritura/ejecución dueño, lectura/ejecución otros)
        mkdir(ruta_directorio, 0755);
    }
}

/*
 * ============================================================================
 * 9. FUNCIÓN: registrar_archivo_salida (Implementada con Llamadas al Sistema)
 * ============================================================================
 * Llamadas al Sistema Utilizadas:
 *   - open(): con bandera O_APPEND para agregar al final sin sobreescribir.
 *   - write(): escribe directamente el bloque de registro.
 *   - close(): cierra el descriptor.
 */
void registrar_archivo_salida(const char *ruta_registro,
                              const char *operacion,
                              const char *ruta_origen,
                              const char *ruta_destino,
                              size_t tamano_origen,
                              long tamano_destino,
                              double porcentaje_ahorro) {
    if (!ruta_registro) return;

    asegurar_directorio_padre(ruta_registro);

    struct stat estado;
    bool es_archivo_nuevo = (stat(ruta_registro, &estado) == -1);

    // LLAMADA AL SISTEMA: open()
    // Bandera O_APPEND: Cada llamada a write() se posiciona atómicamente al final del archivo.
    int fd_bitacora = open(ruta_registro, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_bitacora == -1) {
        perror("Advertencia: No se pudo abrir la bitacora con llamada open()");
        return;
    }

    char mensaje[1024];
    int longitud_mensaje = 0;

    if (es_archivo_nuevo) {
        const char *encabezado = 
            "================================================================================\n"
            "BITÁCORA Y REGISTRO DE ARCHIVOS DE SALIDA - ALGORITMO DE HUFFMAN\n"
            "Implementado con Llamadas al Sistema (open, read, write, close, lseek)\n"
            "Asignatura: Sistemas Operativos (4to Semestre)\n"
            "================================================================================\n\n";
        write(fd_bitacora, encabezado, strlen(encabezado));
    }

    // Consulta de reloj del sistema
    time_t tiempo_actual = time(NULL);
    struct tm *info_tiempo = localtime(&tiempo_actual);
    char texto_fecha[64];
    strftime(texto_fecha, sizeof(texto_fecha), "%Y-%m-%d %H:%M:%S", info_tiempo);

    if (strcmp(operacion, "COMPRESIÓN") == 0) {
        longitud_mensaje = snprintf(mensaje, sizeof(mensaje),
            "[Registro: %s]\n"
            "  • Operación:          %s\n"
            "  • Archivo de Entrada: %s (%zu bytes)\n"
            "  • Archivo de Salida:  %s (%ld bytes)\n"
            "  • Ahorro en Disco:    %.2f%%\n"
            "  • Syscalls de E/S:    open, read, write, close, lseek\n"
            "  • Estado:             EXITOSO\n"
            "--------------------------------------------------------------------------------\n",
            texto_fecha, operacion, ruta_origen, tamano_origen, ruta_destino, tamano_destino, porcentaje_ahorro);
    } else {
        longitud_mensaje = snprintf(mensaje, sizeof(mensaje),
            "[Registro: %s]\n"
            "  • Operación:          %s\n"
            "  • Archivo de Entrada: %s (%zu bytes)\n"
            "  • Archivo de Salida:  %s (%ld bytes)\n"
            "  • Syscalls de E/S:    open, read, write, close\n"
            "  • Estado:             EXITOSO\n"
            "--------------------------------------------------------------------------------\n",
            texto_fecha, operacion, ruta_origen, tamano_origen, ruta_destino, tamano_destino);
    }

    // LLAMADA AL SISTEMA: write()
    if (longitud_mensaje > 0) {
        write(fd_bitacora, mensaje, (size_t)longitud_mensaje);
    }

    // LLAMADA AL SISTEMA: close()
    close(fd_bitacora);
}

/*
 * ============================================================================
 * COMANDO DEL SHELL: huffman
 * ============================================================================
 * El parser de main.c (parse_line) ya separa la línea en argc/argv, así que
 * aquí solo tenemos que leerlos:
 *
 *   huffman comp archivo.txt            ->  argv[1]="comp" argv[2]="archivo.txt"
 *   huffman desc archivo.huf            ->  argv[1]="desc" argv[2]="archivo.huf"
 *   huffman desc archivo.huf salida.txt ->  argv[3]="salida.txt" (opcional)
 *
 * Nombres de salida:
 *   comp: archivo.txt  -> archivo.huf   (se quita la extensión .txt)
 *   desc: archivo.huf  -> archivo.desc  (o el nombre que se pase en argv[3])
 *   El .huf no guarda la extensión original, por eso al descomprimir no se
 *   sabe si era .txt, .c, ... Se puede comparar con: diff archivo.txt archivo.desc
 */

#define RUTA_BITACORA "bitacora_huffman.txt"
#define EXTENSION_COMPRIMIDO ".huf"
#define EXTENSION_DESCOMPRIMIDO ".desc"
#define MAX_RUTA 512

/*
 * ============================================================================
 * PROGRESO Y ESTADO COMPARTIDOS ENTRE HILOS
 * ============================================================================
 * Estas dos variables las ESCRIBE el hilo que comprime (en segundo plano) y las
 * LEE el hilo del editor (para mostrar el % en el prompt). Como dos hilos tocan
 * la misma variable, cada acceso se hace con '#pragma omp atomic', que garantiza
 * que la lectura/escritura ocurre "de una sola vez" (sin quedar a medias).
 */
static int progreso_huffman = 0;   // 0 a 100
static int huffman_ocupado = 0;    // 1 mientras hay una tarea en segundo plano

void huffman_poner_progreso(int porcentaje) {
    #pragma omp atomic write
    progreso_huffman = porcentaje;
}

int huffman_leer_progreso(void) {
    int valor;
    #pragma omp atomic read
    valor = progreso_huffman;
    return valor;
}

int huffman_esta_ocupado(void) {
    int valor;
    #pragma omp atomic read
    valor = huffman_ocupado;
    return valor;
}

/*
 * Imprime una barra de 20 casillas, cada '#' es un 5%. Ejemplo para 40%:
 *   [########............]  40%
 * No imprime salto de línea: el texto del paso se escribe a continuación.
 */
static void imprimir_barra(int porcentaje) {
    int llenas = porcentaje / 5;
    printf("  [");
    for (int i = 0; i < 20; i++) {
        if (i < llenas) {
            printf(COLOR_RESULT "#" COLOR_RESET);
        } else {
            printf(".");
        }
    }
    printf("] %3d%%  ", porcentaje);
}

/* Guarda el progreso (para el editor) y lo dibuja en pantalla */
static void avanzar_progreso(int porcentaje) {
    huffman_poner_progreso(porcentaje);
    imprimir_barra(porcentaje);
}

/*
 * Copia 'ruta' en 'salida' SIN la extensión. Ejemplos:
 *   "notas.txt"        -> "notas"
 *   "docs/informe.md"  -> "docs/informe"
 *   "Makefile"         -> "Makefile"   (no tiene extensión)
 */
void quitar_extension(const char *ruta, char *salida, size_t tamano_salida) {
    snprintf(salida, tamano_salida, "%s", ruta);

    char *ultimo_punto = strrchr(salida, '.');
    char *ultima_barra = strrchr(salida, '/');

    // Solo cortamos si hay un punto DESPUÉS de la última carpeta
    // y no es el primer carácter del nombre (archivos ocultos como ".bashrc")
    if (ultimo_punto == NULL || ultimo_punto == salida) {
        return;
    }
    if (ultima_barra != NULL && ultimo_punto <= ultima_barra + 1) {
        return;
    }
    *ultimo_punto = '\0';   // cortar la cadena justo donde empieza la extensión
}

/* Muestra cómo se usa el comando */
static void mostrar_uso_huffman(void) {
    printf(COLOR_INFO "Uso:\n" COLOR_RESET);
    printf("  " COLOR_PARAM "huffman comp <archivo.txt>" COLOR_RESET "            -> crea <archivo>.huf\n");
    printf("  " COLOR_PARAM "huffman desc <archivo.huf> [salida]" COLOR_RESET "   -> crea <archivo>.desc o [salida]\n");
}

/* Devuelve el tamaño de un archivo en bytes usando la syscall stat(), o -1 si falla */
static long tamano_de_archivo(const char *ruta) {
    struct stat info;
    if (stat(ruta, &info) == -1) {
        return -1;
    }
    return (long)info.st_size;
}

/*
 * Comprime 'ruta_entrada' y guarda el resultado en '<nombre sin extensión>.huf'.
 * Pasos del algoritmo (cada uno avanza la barra de progreso un 20%):
 *   1. Leer todo el archivo a memoria.
 *   2. Contar cuántas veces aparece cada byte.        (PARALELO con OpenMP)
 *   3. Construir el árbol de Huffman con esas frecuencias.
 *   4. Recorrer el árbol para generar el código de cada byte.
 *   5. Escribir cabecera + bits comprimidos en el .huf. (codificación PARALELA)
 */
static int huffman_comprimir(const char *ruta_entrada) {
    char ruta_salida[MAX_RUTA + 8];   // +8: espacio extra para la extensión ".huf"
    char nombre_base[MAX_RUTA];
    quitar_extension(ruta_entrada, nombre_base, sizeof(nombre_base));
    snprintf(ruta_salida, sizeof(ruta_salida), "%s%s", nombre_base, EXTENSION_COMPRIMIDO);

    printf(COLOR_TITLE "\nComprimiendo '%s' -> '%s'\n" COLOR_RESET, ruta_entrada, ruta_salida);
    printf(COLOR_INFO "  (OpenMP: hasta %d hilos para contar frecuencias y codificar)\n" COLOR_RESET,
           omp_get_max_threads() < MAX_HILOS ? omp_get_max_threads() : MAX_HILOS);
    huffman_poner_progreso(0);

    /* Paso 1: leer el archivo */
    size_t tamano_original = 0;
    unsigned char *datos = leer_archivo(ruta_entrada, &tamano_original);
    if (datos == NULL) {
        printf(COLOR_ERROR "No se pudo leer el archivo '%s'.\n" COLOR_RESET, ruta_entrada);
        return -1;
    }
    avanzar_progreso(20);
    printf("Archivo leído: " COLOR_PARAM "%zu bytes" COLOR_RESET "\n", tamano_original);

    /* Paso 2: contar frecuencias */
    uint32_t frecuencias[MAX_SIMBOLOS];
    contar_frecuencias(datos, tamano_original, frecuencias);

    int simbolos_distintos = 0;
    for (int i = 0; i < MAX_SIMBOLOS; i++) {
        if (frecuencias[i] > 0) {
            simbolos_distintos++;
        }
    }
    avanzar_progreso(40);
    printf("Frecuencias contadas: " COLOR_PARAM "%d símbolos distintos" COLOR_RESET "\n",
           simbolos_distintos);

    /* Paso 3: construir el árbol (queda en NULL si el archivo está vacío, y eso está bien) */
    NodoHuffman *raiz = crear_arbol_huffman(frecuencias);
    avanzar_progreso(60);
    printf("Árbol de Huffman construido\n");

    /* Paso 4: generar los códigos. 'static' para no gastar ~66 KB de la pila */
    static Diccionario diccionario;
    generar_codigos(raiz, &diccionario);
    avanzar_progreso(80);
    printf("Códigos generados\n");

    /* Paso 5: escribir el archivo comprimido */
    int resultado = guardar_comprimido(ruta_salida, datos, tamano_original,
                                       frecuencias, &diccionario);

    /* Liberar memoria SIEMPRE, haya salido bien o mal */
    liberar_arbol(raiz);
    free(datos);

    if (resultado != 0) {
        printf(COLOR_ERROR "Error al escribir el archivo comprimido.\n" COLOR_RESET);
        return -1;
    }

    long tamano_comprimido = tamano_de_archivo(ruta_salida);
    double ahorro = 0.0;
    if (tamano_original > 0) {
        ahorro = 100.0 * (1.0 - (double)tamano_comprimido / (double)tamano_original);
    }
    avanzar_progreso(100);
    printf("Archivo comprimido escrito: " COLOR_PARAM "%ld bytes" COLOR_RESET "\n",
           tamano_comprimido);
    printf(COLOR_RESULT "Compresión exitosa. Ahorro: %.2f%%\n" COLOR_RESET, ahorro);

    registrar_archivo_salida(RUTA_BITACORA, "COMPRESIÓN", ruta_entrada, ruta_salida,
                             tamano_original, tamano_comprimido, ahorro);
    return 0;
}

/*
 * Descomprime 'ruta_entrada' (un .huf).
 *   - Si 'ruta_destino' es NULL, la salida es '<nombre sin extensión>.desc'.
 *   - Si no, se usa 'ruta_destino' tal cual (ej: notas.txt).
 * El progreso intermedio lo va actualizando descomprimir_archivo() cada 4 KB.
 */
static int huffman_descomprimir(const char *ruta_entrada, const char *ruta_destino) {
    char ruta_salida[MAX_RUTA + 8];   // +8: espacio extra para la extensión ".desc"

    if (ruta_destino != NULL) {
        snprintf(ruta_salida, sizeof(ruta_salida), "%s", ruta_destino);
    } else {
        char nombre_base[MAX_RUTA];
        quitar_extension(ruta_entrada, nombre_base, sizeof(nombre_base));
        snprintf(ruta_salida, sizeof(ruta_salida), "%s%s", nombre_base, EXTENSION_DESCOMPRIMIDO);
    }

    printf(COLOR_TITLE "\nDescomprimiendo '%s' -> '%s'\n" COLOR_RESET, ruta_entrada, ruta_salida);
    avanzar_progreso(0);
    printf("Iniciando...\n");

    /* descomprimir_archivo hace todo: lee cabecera, reconstruye árbol y decodifica bits */
    if (descomprimir_archivo(ruta_entrada, ruta_salida) != 0) {
        printf(COLOR_ERROR "Error al descomprimir '%s'.\n" COLOR_RESET, ruta_entrada);
        return -1;
    }

    long tamano_comprimido = tamano_de_archivo(ruta_entrada);
    long tamano_restaurado = tamano_de_archivo(ruta_salida);
    avanzar_progreso(100);
    printf("Comprimido: " COLOR_PARAM "%ld bytes" COLOR_RESET
           "  ->  Restaurado: " COLOR_PARAM "%ld bytes" COLOR_RESET "\n",
           tamano_comprimido, tamano_restaurado);
    printf(COLOR_RESULT "Descompresión exitosa.\n" COLOR_RESET);

    registrar_archivo_salida(RUTA_BITACORA, "DESCOMPRESIÓN", ruta_entrada, ruta_salida,
                             (size_t)tamano_comprimido, tamano_restaurado, 0.0);
    return 0;
}

/* Punto de entrada que llama main.c cuando argv[0] es "huffman" */
int cmd_huffman(int argc, char **argv) {
    /* Necesitamos al menos: huffman <modo> <archivo> */
    if (argc < 3) {
        printf(COLOR_ERROR "Faltan argumentos.\n" COLOR_RESET);
        mostrar_uso_huffman();
        return -1;
    }

    const char *modo = argv[1];
    const char *archivo = argv[2];

    if (strcmp(modo, "comp") == 0) {
        return huffman_comprimir(archivo);
    } else if (strcmp(modo, "desc") == 0) {
        /* argv[3] es opcional: nombre del archivo restaurado */
        const char *destino = NULL;
        if (argc >= 4) {
            destino = argv[3];
        }
        return huffman_descomprimir(archivo, destino);
    }

    printf(COLOR_ERROR "Modo '%s' no reconocido.\n" COLOR_RESET, modo);
    mostrar_uso_huffman();
    return -1;
}

/*
 * ============================================================================
 * TAREA EN SEGUNDO PLANO (usada por el editor)
 * ============================================================================
 * Lanza 'huffman <modo> <archivo>' como una TAREA de OpenMP (#pragma omp task).
 * El hilo que llama (el del editor) NO espera: crea la tarea y vuelve de
 * inmediato a pedir comandos. Otro hilo del equipo la ejecuta.
 *
 * IMPORTANTE: debe llamarse dentro de una región 'parallel' (el editor la crea).
 *
 * Devuelve 0 si se lanzó, -1 si ya había otra tarea en curso.
 */
int huffman_en_segundo_plano(const char *modo, const char *archivo) {
    /* Solo una tarea a la vez: la compresión usa arreglos 'static'
       (frecuencias_locales, diccionario) que no se pueden compartir entre dos tareas */
    if (huffman_esta_ocupado()) {
        printf(COLOR_ERROR "Ya hay una tarea Huffman en curso (%d%%). Espera a que termine.\n" COLOR_RESET,
               huffman_leer_progreso());
        return -1;
    }

    /* Copias propias del texto: el búfer del editor se reutiliza en el siguiente
       comando, así que la tarea no puede depender de él */
    char modo_tarea[8];
    char archivo_tarea[MAX_RUTA];
    snprintf(modo_tarea, sizeof(modo_tarea), "%s", modo);
    snprintf(archivo_tarea, sizeof(archivo_tarea), "%s", archivo);

    /* Marcamos "ocupado" ANTES de crear la tarea */
    #pragma omp atomic write
    huffman_ocupado = 1;
    huffman_poner_progreso(0);

    printf(COLOR_INFO "Tarea Huffman lanzada en segundo plano. Puedes seguir usando el editor.\n" COLOR_RESET);

    /* firstprivate: la tarea recibe su PROPIA copia de los dos arreglos */
    #pragma omp task firstprivate(modo_tarea, archivo_tarea)
    {
        char *args[] = { "huffman", modo_tarea, archivo_tarea, NULL };
        cmd_huffman(3, args);

        /* Al terminar (bien o mal) liberamos el "ocupado" */
        #pragma omp atomic write
        huffman_ocupado = 0;

        printf(COLOR_PROMPT "editor> " COLOR_RESET);   // re-dibujar el prompt
        fflush(stdout);
    }

    return 0;
}
