#include "shell.h"
#include <unistd.h>      /* read, write, lseek, ftruncate, close */
#include <fcntl.h>       /* open, O_RDWR, O_CREAT */
#include <sys/types.h>   /* off_t, ssize_t */
#include <string.h>      /* strlen, strcpy, strstr */
#include <stdlib.h>      /* malloc, free */
#include <omp.h>         /* omp_set_max_active_levels (tarea Huffman en segundo plano) */

/**
 * ====================================================================================
 * MÓDULO: EDITOR DE TEXTO POR LÍNEAS (categoría "edicion")
 * ====================================================================================
 * Este módulo agrega al shell educativo un pequeño editor de texto que guarda el
 * contenido en una LISTA ENLAZADA SIMPLE (una línea de texto por nodo).
 *
 * Idea general (para estudiantes nuevos en C):
 *   - La lista enlazada que vive en memoria RAM es la "fuente de la verdad".
 *   - Cada vez que modificamos la lista (agregar, insertar o borrar una línea),
 *     volvemos a escribir TODO el contenido en el archivo del disco.
 *   - Para tocar el disco usamos SOLO llamadas al sistema de bajo nivel:
 *       open, read, write, lseek, ftruncate y close.
 *     (Está PROHIBIDO usar fopen/fread/fwrite/fclose en este proyecto.)
 *
 * El editor funciona como un "mini shell" dentro del shell: muestra su propio
 * prompt "editor> " y acepta comandos de una sola letra hasta que escribimos 'q'.
 *
 * Subcomandos disponibles:
 *   o [archivo]      Abre o crea un archivo y carga su contenido en la lista.
 *   a [texto]        Agrega una línea nueva al final.
 *   p                Imprime todo el texto (líneas numeradas desde 1).
 *   p [n]            Imprime solo la línea número n.
 *   d [n]            Elimina la línea número n.
 *   i [n] [texto]    Inserta "texto" como nueva línea en la posición n.
 *   s [palabra]      Busca una palabra y muestra en qué líneas aparece.
 *   c [archivo]      Comprime con Huffman (sin archivo: el que está abierto).
 *   x [archivo.huf]  Descomprime con Huffman (sin archivo: <abierto>.huf).
 *   h                Muestra la ayuda del editor.
 *   q                Guarda, cierra el archivo, libera memoria y vuelve al shell.
 */

/* Longitud máxima de una línea de entrada del teclado dentro del editor */
#define EDITOR_MAX_LINEA 2048

/**
 * ------------------------------------------------------------------------------------
 * Estructura del nodo de la lista enlazada.
 * ------------------------------------------------------------------------------------
 * Cada nodo guarda UNA línea de texto (sin el salto de línea '\n') y un puntero
 * al siguiente nodo. El último nodo apunta a NULL.
 */
typedef struct Linea {
    char *texto;          /* línea SIN el '\n', terminada en '\0' */
    struct Linea *sig;    /* puntero al siguiente nodo (NULL si es el último) */
} Linea;

/* ====================================================================================
 * FUNCIONES AUXILIARES DE LA LISTA ENLAZADA
 * ==================================================================================== */

/**
 * Crea un nodo nuevo copiando el texto recibido.
 * Reserva su propia memoria con malloc para que la línea no dependa del buffer
 * original del teclado. Devuelve NULL si no hay memoria.
 */
Linea *nueva_linea(const char *texto) {
    Linea *nodo = (Linea *)malloc(sizeof(Linea));
    if (nodo == NULL) {
        return NULL;
    }
    /* +1 para reservar espacio para el terminador '\0' */
    nodo->texto = (char *)malloc(strlen(texto) + 1);
    if (nodo->texto == NULL) {
        free(nodo);
        return NULL;
    }
    strcpy(nodo->texto, texto);
    nodo->sig = NULL;
    return nodo;
}

/**
 * Agrega una línea nueva AL FINAL de la lista.
 * Recibe la dirección de la cabeza (Linea **) porque si la lista está vacía
 * debemos poder modificar el puntero cabeza del llamador.
 */
void agregar_final(Linea **cabeza, const char *texto) {
    Linea *nodo = nueva_linea(texto);
    if (nodo == NULL) {
        printf(COLOR_ERROR "Error: sin memoria para agregar la línea.\n" COLOR_RESET);
        return;
    }
    if (*cabeza == NULL) {
        /* La lista estaba vacía: el nodo nuevo pasa a ser la cabeza */
        *cabeza = nodo;
        return;
    }
    /* Recorrer hasta el último nodo y enlazar el nuevo al final */
    Linea *actual = *cabeza;
    while (actual->sig != NULL) {
        actual = actual->sig;
    }
    actual->sig = nodo;
}

/**
 * Cuenta cuántas líneas (nodos) tiene la lista. Útil para validar rangos.
 */
int contar_lineas(Linea *cabeza) {
    int total = 0;
    Linea *actual = cabeza;
    while (actual != NULL) {
        total++;
        actual = actual->sig;
    }
    return total;
}

/**
 * Inserta "texto" como una línea nueva en la posición n (contando desde 1),
 * desplazando hacia abajo el resto de las líneas.
 *   - n = 1 inserta al inicio.
 *   - n = (total + 1) inserta al final.
 * Devuelve 1 si lo logró, 0 si la posición es inválida.
 */
int insertar_en(Linea **cabeza, int n, const char *texto) {
    int total = contar_lineas(*cabeza);

    /* La posición válida va de 1 hasta total+1 (una después de la última) */
    if (n < 1 || n > total + 1) {
        return 0;
    }

    Linea *nodo = nueva_linea(texto);
    if (nodo == NULL) {
        printf(COLOR_ERROR "Error: sin memoria para insertar la línea.\n" COLOR_RESET);
        return 0;
    }

    if (n == 1) {
        /* Insertar al inicio: el nodo nuevo apunta a la antigua cabeza */
        nodo->sig = *cabeza;
        *cabeza = nodo;
        return 1;
    }

    /* Avanzar hasta el nodo anterior a la posición n (el nodo n-1) */
    Linea *anterior = *cabeza;
    for (int i = 1; i < n - 1; i++) {
        anterior = anterior->sig;
    }
    /* Enlazar: anterior -> nodo -> (lo que seguía a anterior) */
    nodo->sig = anterior->sig;
    anterior->sig = nodo;
    return 1;
}

/**
 * Elimina la línea número n (contando desde 1) y libera su memoria.
 * Devuelve 1 si la eliminó, 0 si n está fuera de rango.
 */
int eliminar_linea(Linea **cabeza, int n) {
    int total = contar_lineas(*cabeza);
    if (n < 1 || n > total) {
        return 0;
    }

    Linea *a_borrar;

    if (n == 1) {
        /* Borrar la cabeza: la nueva cabeza es el segundo nodo */
        a_borrar = *cabeza;
        *cabeza = (*cabeza)->sig;
    } else {
        /* Avanzar hasta el nodo anterior (n-1) para "saltar" el nodo n */
        Linea *anterior = *cabeza;
        for (int i = 1; i < n - 1; i++) {
            anterior = anterior->sig;
        }
        a_borrar = anterior->sig;
        anterior->sig = a_borrar->sig;
    }

    /* Liberar primero el texto y luego el nodo (evita fugas de memoria) */
    free(a_borrar->texto);
    free(a_borrar);
    return 1;
}

/**
 * Imprime el contenido de la lista.
 *   - Si n <= 0 imprime TODAS las líneas numeradas.
 *   - Si n >= 1 imprime solo la línea n.
 */
void imprimir(Linea *cabeza, int n) {
    if (cabeza == NULL) {
        printf(COLOR_INFO "(el documento está vacío)\n" COLOR_RESET);
        return;
    }

    int numero = 1;
    Linea *actual = cabeza;
    while (actual != NULL) {
        if (n <= 0 || numero == n) {
            printf(COLOR_PARAM "%4d" COLOR_RESET " | %s\n", numero, actual->texto);
        }
        numero++;
        actual = actual->sig;
    }

    if (n >= 1 && n > numero - 1) {
        printf(COLOR_ERROR "No existe la línea %d (el documento tiene %d líneas).\n" COLOR_RESET,
               n, numero - 1);
    }
}

/**
 * Busca una palabra dentro de cada línea usando strstr (coincidencia de subcadena)
 * e imprime el número y el contenido de cada línea donde aparece.
 * Es una operación de SOLO LECTURA: no modifica la lista ni el disco.
 */
void buscar(Linea *cabeza, const char *palabra) {
    int numero = 1;
    int encontrados = 0;
    Linea *actual = cabeza;

    while (actual != NULL) {
        if (strstr(actual->texto, palabra) != NULL) {
            printf(COLOR_RESULT "%4d" COLOR_RESET " | %s\n", numero, actual->texto);
            encontrados++;
        }
        numero++;
        actual = actual->sig;
    }

    if (encontrados == 0) {
        printf(COLOR_INFO "No se encontró '%s' en ninguna línea.\n" COLOR_RESET, palabra);
    } else {
        printf(COLOR_INFO "Se encontró '%s' en %d línea(s).\n" COLOR_RESET, palabra, encontrados);
    }
}

/**
 * Libera TODA la lista enlazada (cada texto y cada nodo) y deja la cabeza en NULL.
 * Así no quedan fugas de memoria al salir del editor.
 */
void liberar_lista(Linea **cabeza) {
    Linea *actual = *cabeza;
    while (actual != NULL) {
        Linea *siguiente = actual->sig; /* guardar el siguiente antes de liberar */
        free(actual->texto);
        free(actual);
        actual = siguiente;
    }
    *cabeza = NULL;
}

/* ====================================================================================
 * SINCRONIZACIÓN CON EL DISCO (lseek + write + ftruncate)
 * ==================================================================================== */

/**
 * Escribe TODO el contenido de la lista en el archivo abierto (descriptor fd).
 *
 * Pasos (y por qué):
 *   1. lseek(fd, 0, SEEK_SET): mover el "cursor" del archivo al inicio para
 *      sobrescribir desde el principio.
 *   2. write(): escribir cada línea seguida de un '\n', contando los bytes totales.
 *   3. ftruncate(fd, total): recortar el archivo al tamaño exacto escrito. Esto es
 *      indispensable cuando el documento se hizo MÁS CORTO (por ejemplo tras un 'd'),
 *      porque si no, quedarían bytes viejos "colgando" al final del archivo.
 */
void guardar_en_disco(int fd, Linea *cabeza) {
    if (fd < 0) {
        return; /* No hay archivo abierto: nada que guardar */
    }

    /* 1. Posicionar el cursor del archivo al inicio */
    LOG_SYSCALL("lseek", "%d, 0, SEEK_SET", fd);
    off_t pos = lseek(fd, 0, SEEK_SET);
    if (pos == -1) {
        LOG_SYSCALL_ERROR("error");
        return;
    }
    LOG_SYSCALL_RESULT(pos);

    /* 2. Escribir cada línea + su salto de línea, acumulando el total de bytes */
    long total = 0;
    Linea *actual = cabeza;
    while (actual != NULL) {
        int largo = (int)strlen(actual->texto);

        LOG_SYSCALL("write", "%d, \"%s\", %d", fd, actual->texto, largo);
        ssize_t escritos = write(fd, actual->texto, largo);
        if (escritos == -1) {
            LOG_SYSCALL_ERROR("error");
            return;
        }
        LOG_SYSCALL_RESULT(escritos);
        total += escritos;

        /* Escribir el salto de línea que separa una línea de la siguiente */
        LOG_SYSCALL("write", "%d, \"\\n\", 1", fd);
        ssize_t salto = write(fd, "\n", 1);
        if (salto == -1) {
            LOG_SYSCALL_ERROR("error");
            return;
        }
        LOG_SYSCALL_RESULT(salto);
        total += salto;

        actual = actual->sig;
    }

    /* 3. Recortar el archivo al tamaño exacto escrito */
    LOG_SYSCALL("ftruncate", "%d, %ld", fd, total);
    if (ftruncate(fd, (off_t)total) == -1) {
        LOG_SYSCALL_ERROR("error");
        return;
    }
    LOG_SYSCALL_RESULT(0);
}

/* ====================================================================================
 * APERTURA Y CARGA DESDE EL DISCO (open + read)
 * ==================================================================================== */

/**
 * Abre (o crea) el archivo indicado y carga su contenido en una lista enlazada nueva.
 *
 * Parámetros:
 *   - nombre: ruta del archivo.
 *   - fd_salida: por aquí se devuelve el descriptor de archivo abierto.
 *
 * Devuelve la cabeza de la lista construida (puede ser NULL si el archivo está vacío).
 * Si open falla, deja *fd_salida en -1 y devuelve NULL.
 */
Linea *abrir_archivo(const char *nombre, int *fd_salida) {
    Linea *cabeza = NULL;

    /* Abrir en lectura/escritura; crear si no existe, con permisos 0644 (rw-r--r--) */
    LOG_SYSCALL("open", "\"%s\", O_RDWR|O_CREAT, 0644", nombre);
    int fd = open(nombre, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        LOG_SYSCALL_ERROR("error");
        *fd_salida = -1;
        return NULL;
    }
    LOG_SYSCALL_RESULT(fd);
    *fd_salida = fd;

    /*
     * Leer el archivo por bloques hacia un buffer dinámico que va creciendo.
     * Usamos un buffer temporal fijo y lo vamos agregando al buffer total con realloc.
     */
    char temporal[1024];
    char *contenido = NULL;   /* buffer dinámico con todo el archivo */
    long tamano = 0;          /* bytes acumulados en 'contenido' */
    ssize_t leidos;

    while (1) {
        /* Mostramos la syscall ANTES de ejecutarla y su resultado después,
           incluyendo la lectura final que devuelve 0 (fin de archivo, EOF). */
        LOG_SYSCALL("read", "%d, buffer, %d", fd, (int)sizeof(temporal));
        leidos = read(fd, temporal, sizeof(temporal));
        if (leidos == -1) {
            LOG_SYSCALL_ERROR("error");
            break;
        }
        LOG_SYSCALL_RESULT(leidos);

        if (leidos == 0) {
            break; /* EOF: ya no hay más bytes que leer */
        }

        char *mas_grande = (char *)realloc(contenido, tamano + leidos);
        if (mas_grande == NULL) {
            printf(COLOR_ERROR "Error: sin memoria al leer el archivo.\n" COLOR_RESET);
            free(contenido);
            return NULL; /* fd queda abierto; el llamador decidirá cerrarlo */
        }
        contenido = mas_grande;
        memcpy(contenido + tamano, temporal, leidos);
        tamano += leidos;
    }

    /*
     * Convertir el buffer de bytes en nodos de la lista, separando por '\n'.
     * Cada línea se copia a un pequeño buffer y se agrega con agregar_final.
     */
    long inicio = 0;
    for (long i = 0; i < tamano; i++) {
        if (contenido[i] == '\n') {
            long largo = i - inicio;
            char *linea = (char *)malloc(largo + 1);
            if (linea != NULL) {
                memcpy(linea, contenido + inicio, largo);
                linea[largo] = '\0';
                agregar_final(&cabeza, linea);
                free(linea);
            }
            inicio = i + 1;
        }
    }
    /* Si el archivo no termina en '\n', agregar la última línea pendiente */
    if (inicio < tamano) {
        long largo = tamano - inicio;
        char *linea = (char *)malloc(largo + 1);
        if (linea != NULL) {
            memcpy(linea, contenido + inicio, largo);
            linea[largo] = '\0';
            agregar_final(&cabeza, linea);
            free(linea);
        }
    }

    free(contenido);
    return cabeza;
}

/**
 * Muestra la lista de subcomandos disponibles dentro del editor.
 */
void ayuda_editor(void) {
    printf(COLOR_TITLE "\n--- Comandos del editor de texto ---\n" COLOR_RESET);
    printf("  " COLOR_PROMPT "o [archivo]" COLOR_RESET "      Abre o crea un archivo y carga su contenido.\n");
    printf("  " COLOR_PROMPT "a [texto]" COLOR_RESET "        Agrega una línea al final.\n");
    printf("  " COLOR_PROMPT "p" COLOR_RESET "                Imprime todo el documento.\n");
    printf("  " COLOR_PROMPT "p [n]" COLOR_RESET "            Imprime solo la línea n.\n");
    printf("  " COLOR_PROMPT "d [n]" COLOR_RESET "            Elimina la línea n.\n");
    printf("  " COLOR_PROMPT "i [n] [texto]" COLOR_RESET "    Inserta texto en la posición n.\n");
    printf("  " COLOR_PROMPT "s [palabra]" COLOR_RESET "      Busca una palabra en el documento.\n");
    printf("  " COLOR_PROMPT "c [archivo]" COLOR_RESET "      Comprime con Huffman (sin archivo: el abierto).\n");
    printf("  " COLOR_PROMPT "x [archivo.huf]" COLOR_RESET "  Descomprime con Huffman (sin archivo: <abierto>.huf).\n");
    printf("  " COLOR_PROMPT "h" COLOR_RESET "                Muestra esta ayuda.\n");
    printf("  " COLOR_PROMPT "q" COLOR_RESET "                Guarda, cierra y vuelve al shell.\n\n");
}

/* ====================================================================================
 * SEPARADOR DE ARGUMENTOS DENTRO DEL EDITOR
 * ==================================================================================== */

/**
 * Separa la línea del editor en: comando (primera palabra) y "resto" (lo que sigue).
 * Ejemplo: "i 2 Hola mundo"  ->  comando='i', numero_texto apunta a "2 Hola mundo".
 *
 * Devolvemos el resto SIN partir, porque comandos como 'a' e 'i' necesitan conservar
 * los espacios del texto del usuario tal cual los escribió.
 */
static char *saltar_espacios(char *p) {
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

/* ====================================================================================
 * COMANDO PRINCIPAL: editor
 * ====================================================================================
 * Este es el "handler" que el shell llama cuando el usuario escribe:
 *     editor            (abre el editor sin archivo)
 *     editor notas.txt  (abre el editor y carga notas.txt)
 *
 * Abre un sub-REPL propio con el prompt "editor> " hasta que el usuario escribe 'q'.
 */
int cmd_editor(int argc, char **argv) {
    Linea *cabeza = NULL;     /* cabeza de la lista enlazada (el documento en memoria) */
    int fd = -1;              /* descriptor del archivo abierto (-1 = ninguno) */
    char nombre[256] = "";    /* nombre del archivo actualmente abierto */
    char linea[EDITOR_MAX_LINEA];

    printf(COLOR_TITLE "\n=== Editor de texto (lista enlazada) ===\n" COLOR_RESET);
    printf(COLOR_INFO "Escribe 'h' para ver los comandos, 'q' para salir.\n" COLOR_RESET);

    /* Si el usuario pasó un archivo (editor notas.txt), abrirlo de una vez */
    if (argc > 1) {
        cabeza = abrir_archivo(argv[1], &fd);
        if (fd != -1) {
            strncpy(nombre, argv[1], sizeof(nombre) - 1);
            nombre[sizeof(nombre) - 1] = '\0';
            printf(COLOR_RESULT "Archivo '%s' abierto (%d líneas).\n" COLOR_RESET,
                   nombre, contar_lineas(cabeza));
        }
    }

    /*
     * ---------------------- Hilos del editor (OpenMP) ----------------------
     * Creamos un equipo de 2 hilos:
     *   - El hilo que entra al 'single' ejecuta el bucle del editor (lee comandos).
     *   - El otro hilo se queda esperando en la barrera del 'single' y, mientras
     *     espera, ejecuta las tareas (#pragma omp task) que crea el editor con
     *     los comandos 'c' y 'x'. Ese es nuestro "background worker".
     *
     * omp_set_max_active_levels(2) permite que DENTRO de la tarea la compresión
     * cree su propio equipo de hilos (parallel for de contar_frecuencias y de
     * guardar_comprimido). Sin esto, la compresión correría con 1 solo hilo.
     */
    omp_set_max_active_levels(2);

    #pragma omp parallel num_threads(2)
    {
        #pragma omp single
        {
            /* ------------------------- Bucle REPL del editor ------------------------- */
            while (1) {
                /* Si hay una tarea Huffman en curso, el prompt muestra su progreso */
                if (huffman_esta_ocupado()) {
                    printf(COLOR_PROMPT "editor [huffman %d%%]> " COLOR_RESET, huffman_leer_progreso());
                } else {
                    printf(COLOR_PROMPT "editor> " COLOR_RESET);
                }
                fflush(stdout);

                if (fgets(linea, sizeof(linea), stdin) == NULL) {
                    printf("\n"); /* Ctrl+D: salir del editor */
                    break;
                }

                /* Quitar el salto de línea final que deja fgets */
                linea[strcspn(linea, "\n")] = '\0';

                /* La primera letra (ignorando espacios) es el comando */
                char *p = saltar_espacios(linea);
                if (*p == '\0') {
                    continue; /* línea vacía: pedir otro comando */
                }

                char comando = *p;
                /* "resto" es lo que viene después del comando y de los espacios */
                char *resto = saltar_espacios(p + 1);

                /* ------------- Protección contra condiciones de carrera ------------- */
                /* Mientras Huffman trabaja en segundo plano, el otro hilo puede estar
                   LEYENDO el archivo. Si aquí lo sobrescribimos (a, d, i), cambiamos
                   de archivo (o) o lanzamos otra tarea (c, x), habría una carrera.
                   Por eso esos comandos se bloquean. Se permiten p, s, h y q,
                   que solo leen la lista en memoria (q espera con taskwait). */
                if (huffman_esta_ocupado() &&
                    (comando == 'a' || comando == 'd' || comando == 'i' ||
                     comando == 'o' || comando == 'c' || comando == 'x')) {
                    printf(COLOR_ERROR "No se puede usar '%c' mientras Huffman está trabajando (%d%%).\n" COLOR_RESET,
                           comando, huffman_leer_progreso());
                    printf(COLOR_INFO "Mientras tanto puedes usar: p, s, h o q.\n" COLOR_RESET);
                    continue;
                }

                /* ----------------------------- q: salir ----------------------------- */
                if (comando == 'q') {
                    break;
                }
                /* ----------------------------- h: ayuda ----------------------------- */
                else if (comando == 'h') {
                    ayuda_editor();
                }
                /* -------------------------- o: abrir archivo ------------------------- */
                else if (comando == 'o') {
                    if (*resto == '\0') {
                        printf(COLOR_ERROR "Uso: o [archivo]\n" COLOR_RESET);
                        continue;
                    }
                    /* Cerrar el archivo anterior y liberar la lista actual (evita fugas) */
                    if (fd != -1) {
                        LOG_SYSCALL("close", "%d", fd);
                        LOG_SYSCALL_RESULT(close(fd));
                        fd = -1;
                    }
                    liberar_lista(&cabeza);

                    cabeza = abrir_archivo(resto, &fd);
                    if (fd != -1) {
                        strncpy(nombre, resto, sizeof(nombre) - 1);
                        nombre[sizeof(nombre) - 1] = '\0';
                        printf(COLOR_RESULT "Archivo '%s' abierto (%d líneas).\n" COLOR_RESET,
                               nombre, contar_lineas(cabeza));
                    }
                }
                /* ------------------------ a: agregar al final ------------------------ */
                else if (comando == 'a') {
                    if (*resto == '\0') {
                        printf(COLOR_ERROR "Uso: a [texto]\n" COLOR_RESET);
                        continue;
                    }
                    agregar_final(&cabeza, resto);
                    guardar_en_disco(fd, cabeza);
                    printf(COLOR_RESULT "Línea agregada.\n" COLOR_RESET);
                }
                /* ----------------------------- p: imprimir --------------------------- */
                else if (comando == 'p') {
                    if (*resto == '\0') {
                        imprimir(cabeza, 0);      /* imprimir todo */
                    } else {
                        imprimir(cabeza, atoi(resto)); /* imprimir una línea */
                    }
                }
                /* --------------------------- d: eliminar n --------------------------- */
                else if (comando == 'd') {
                    if (*resto == '\0') {
                        printf(COLOR_ERROR "Uso: d [n]\n" COLOR_RESET);
                        continue;
                    }
                    int n = atoi(resto);
                    if (eliminar_linea(&cabeza, n)) {
                        guardar_en_disco(fd, cabeza);
                        printf(COLOR_RESULT "Línea %d eliminada.\n" COLOR_RESET, n);
                    } else {
                        printf(COLOR_ERROR "No existe la línea %d.\n" COLOR_RESET, n);
                    }
                }
                /* ------------------------- i: insertar en n -------------------------- */
                else if (comando == 'i') {
                    /* Formato esperado: i [n] [texto]. Primero leemos el número. */
                    if (*resto == '\0') {
                        printf(COLOR_ERROR "Uso: i [n] [texto]\n" COLOR_RESET);
                        continue;
                    }
                    int n = atoi(resto);

                    /* Avanzar más allá del número para quedarnos con el texto */
                    char *texto = resto;
                    while (*texto != ' ' && *texto != '\t' && *texto != '\0') {
                        texto++; /* saltar los dígitos del número */
                    }
                    texto = saltar_espacios(texto);

                    if (*texto == '\0') {
                        printf(COLOR_ERROR "Uso: i [n] [texto]\n" COLOR_RESET);
                        continue;
                    }
                    if (insertar_en(&cabeza, n, texto)) {
                        guardar_en_disco(fd, cabeza);
                        printf(COLOR_RESULT "Línea insertada en la posición %d.\n" COLOR_RESET, n);
                    } else {
                        printf(COLOR_ERROR "Posición %d inválida (usa de 1 a %d).\n" COLOR_RESET,
                               n, contar_lineas(cabeza) + 1);
                    }
                }
                /* ---------------------------- s: buscar ------------------------------ */
                else if (comando == 's') {
                    if (*resto == '\0') {
                        printf(COLOR_ERROR "Uso: s [palabra]\n" COLOR_RESET);
                        continue;
                    }
                    buscar(cabeza, resto);
                }
                /* ------------------------ c: comprimir (Huffman) --------------------- */
                else if (comando == 'c') {
                    /* ¿Qué archivo? El que escribió el usuario, o el que está abierto */
                    char *archivo = resto;
                    if (*archivo == '\0') {
                        if (fd == -1) {
                            printf(COLOR_ERROR "Uso: c [archivo]  (o abre uno primero con 'o')\n" COLOR_RESET);
                            continue;
                        }
                        archivo = nombre;
                    }

                    /* Si es el archivo abierto, guardamos primero lo que hay en memoria,
                       para que se comprima exactamente lo que el usuario ve en el editor */
                    if (fd != -1 && strcmp(archivo, nombre) == 0) {
                        guardar_en_disco(fd, cabeza);
                    }

                    /* Lanzar "huffman comp <archivo>" en segundo plano (omp task).
                       El editor NO espera: vuelve de inmediato a pedir comandos. */
                    huffman_en_segundo_plano("comp", archivo);
                }
                /* ----------------------- x: descomprimir (Huffman) ------------------- */
                else if (comando == 'x') {
                    char archivo_huf[300];
                    if (*resto != '\0') {
                        snprintf(archivo_huf, sizeof(archivo_huf), "%s", resto);
                    } else if (fd != -1) {
                        /* Sin argumento: descomprimir el .huf del archivo abierto.
                           notas.txt -> se busca notas.huf */
                        char nombre_base[256];
                        quitar_extension(nombre, nombre_base, sizeof(nombre_base));
                        snprintf(archivo_huf, sizeof(archivo_huf), "%s.huf", nombre_base);
                    } else {
                        printf(COLOR_ERROR "Uso: x [archivo.huf]  (o abre uno primero con 'o')\n" COLOR_RESET);
                        continue;
                    }

                    /* Lanzar "huffman desc <archivo.huf>" en segundo plano (omp task) */
                    huffman_en_segundo_plano("desc", archivo_huf);
                }
                /* ------------------------- comando desconocido ----------------------- */
                else {
                    printf(COLOR_ERROR "Comando '%c' no reconocido. Escribe 'h' para la ayuda.\n" COLOR_RESET,
                           comando);
                }
            }

            /* Antes de cerrar el archivo hay que esperar a que la tarea termine
               (equivale a un "join" del hilo en segundo plano) */
            if (huffman_esta_ocupado()) {
                printf(COLOR_INFO "Esperando a que termine la tarea Huffman en segundo plano...\n" COLOR_RESET);
            }
            #pragma omp taskwait
        } /* fin del single (aquí hay una barrera implícita) */
    } /* fin de la región paralela: los 2 hilos terminan aquí */

    /* ------------------------- Cierre limpio del editor ------------------------- */
    /* Guardado final (por si quedó algo sin sincronizar) y cierre del descriptor */
    if (fd != -1) {
        guardar_en_disco(fd, cabeza);
        LOG_SYSCALL("close", "%d", fd);
        LOG_SYSCALL_RESULT(close(fd));
        fd = -1;
    }
    /* Liberar toda la memoria de la lista para no dejar fugas */
    liberar_lista(&cabeza);

    printf(COLOR_INFO "Saliendo del editor. Volviendo al shell.\n" COLOR_RESET);
    return 0;
}
