#ifndef SHELL_H
#define SHELL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * ====================================================================================
 * Códigos de Escape ANSI para Colores en Terminal
 * ====================================================================================
 * Permiten imprimir texto en la consola de comandos usando colores y formatos.
 * Cada macro representa una secuencia de escape que el emulador de terminal interpreta
 * para cambiar el color del lápiz de dibujo. COLOR_RESET devuelve el color a la normalidad.
 */
#define COLOR_RESET     "\033[0m"      /* Restablece todos los atributos de color y estilo */
#define COLOR_PROMPT    "\033[1;36m"   /* Cian brillante: Usado para el prompt interactivo 'sys-shell>' */
#define COLOR_SYSCALL   "\033[1;35m"   /* Púrpura/Magenta brillante: Resalta el nombre de las syscalls ejecutadas */
#define COLOR_PARAM     "\033[0;33m"   /* Amarillo/Marrón: Usado para valores numéricos o parámetros importantes */
#define COLOR_RESULT    "\033[1;32m"   /* Verde brillante: Destaca valores de retorno exitosos (>= 0 o punteros válidos) */
#define COLOR_ERROR     "\033[1;31m"   /* Rojo brillante: Resalta fallos y descripciones de error (errno) */
#define COLOR_INFO      "\033[0;90m"   /* Gris oscuro: Texto de depuración o comentarios explicativos menores */
#define COLOR_TITLE     "\033[1;97m"   /* Blanco brillante en negrita: Para encabezados de tablas y banners */
#define COLOR_CATEGORY  "\033[1;34m"   /* Azul brillante: Categorías del sistema en el comando 'help' */

/**
 * ====================================================================================
 * Macros Educativas de Trazado de Llamadas al Sistema (Syscalls)
 * ====================================================================================
 * Estas macros imitan el comportamiento de la herramienta 'strace' de Linux.
 * Al ejecutarse una syscall en el código, se llama a LOG_SYSCALL con el nombre
 * de la función y sus argumentos para mostrarlos formateados.
 * Dependiendo del resultado, se invocará LOG_SYSCALL_RESULT o LOG_SYSCALL_ERROR.
 */

/* Imprime la llamada al sistema y sus parámetros */
#define LOG_SYSCALL(syscall_name, format, ...) \
    printf(COLOR_SYSCALL "[syscall] " syscall_name COLOR_RESET "(" format ") ... " COLOR_RESET, ##__VA_ARGS__)

/* Imprime el valor de retorno en verde cuando es un número entero exitoso (e.g., File Descriptor o bytes) */
#define LOG_SYSCALL_RESULT(result) \
    printf("= " COLOR_RESULT "%ld" COLOR_RESET "\n", (long)(result))

/* Imprime la dirección de memoria de retorno en verde cuando se retorna un puntero (e.g., mmap, sbrk) */
#define LOG_SYSCALL_RESULT_PTR(result) \
    printf("= " COLOR_RESULT "%p" COLOR_RESET "\n", (void*)(result))

/* Imprime el valor -1 en rojo indicando un error y adjunta el texto descriptivo del código errno */
#define LOG_SYSCALL_ERROR(err_name) \
    printf("= " COLOR_ERROR "-1 (%s)" COLOR_RESET "\n", err_name)

/**
 * ====================================================================================
 * Estructura de Registro de Comandos
 * ====================================================================================
 * Define un comando del shell con toda su metadata con fines pedagógicos.
 * Permite listar automáticamente los comandos asociados a syscalls de forma dinámica.
 */
typedef struct {
    const char *name;        /* Nombre textual del comando que el usuario escribe (e.g., 'd_create') */
    const char *category;    /* Categoría de ordenación: "datos", "memoria", "monitoreo", "utilidades" */
    const char *usage;       /* Sintaxis de uso del comando para mostrar en caso de error */
    const char *description; /* Explicación en español de lo que hace el comando a nivel lógico */
    const char *syscalls;    /* Explicación de las syscalls involucradas que se mostrarán en la ayuda */
    int (*handler)(int argc, char **argv); /* Puntero a la función que implementa la lógica del comando */
} Command;

/* ====================================================================================
 * Declaración de Prototipos de Comandos
 * ====================================================================================
 */

/* --- Categoría: Datos y Archivos (cat_datos.c) --- */
int cmd_d_create(int argc, char **argv); /* Syscalls: open, write, close */
int cmd_d_read(int argc, char **argv);   /* Syscalls: open, read, close */
int cmd_d_info(int argc, char **argv);   /* Syscalls: stat */
int cmd_d_copy(int argc, char **argv);   /* Syscalls: open, read, write, close */

/* --- Categoría: Memoria (cat_memoria.c) --- */
int cmd_m_sbrk(int argc, char **argv);   /* Syscalls: sbrk / brk */
int cmd_m_mmap(int argc, char **argv);   /* Syscalls: mmap, munmap */
int cmd_m_info(int argc, char **argv);   /* Mecanismo: Lectura de /proc/self/status */

/* --- Categoría: Monitoreo/Procesos (cat_monitoreo.c) --- */
int cmd_p_fork(int argc, char **argv);    /* Syscalls: fork, getpid, getppid, waitpid */
int cmd_p_exec(int argc, char **argv);    /* Syscalls: fork, execvp, waitpid */
int cmd_p_kill(int argc, char **argv);    /* Syscalls: kill */
int cmd_p_monitor(int argc, char **argv); /* Syscalls: getrusage */

/* --- Categoría: Utilidades (cat_util.c) --- */
int cmd_saludar(int argc, char **argv);   /* Syscalls: getuid */
int cmd_despedir(int argc, char **argv);  /* Syscalls: getuid */
int cmd_hora(int argc, char **argv);      /* Syscalls: time */
int cmd_fecha(int argc, char **argv);     /* Syscalls: time */

/* --- Categoría: Edición (cat_editor.c) --- */
int cmd_editor(int argc, char **argv);    /* Syscalls: open, read, write, lseek, ftruncate, close */

/* ====================================================================================
 * Categoría: Huffman (cat_huffman.c)
 * ====================================================================================
 * Tipos de datos y funciones del algoritmo de compresión de Huffman.
 */
#include <stdint.h>   /* uint16_t, uint32_t, uint64_t */
#include <stdbool.h>  /* bool, true, false */
#include <unistd.h>   /* read, write, lseek, close */
#include <fcntl.h>    /* open, O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC, O_APPEND */
#include <sys/stat.h> /* stat, mkdir */

#define MAX_SIMBOLOS 256            /* Un símbolo por cada valor posible de un byte (0..255) */
#define MAX_LONGITUD_CODIGO 257     /* Un código puede medir hasta 255 bits + '\0' */
#define NUMERO_MAGICO_HUFFMAN 0x46465548u  /* "HUFF" en ASCII: identifica nuestros archivos .huf */

/* Nodo del árbol de Huffman. Las hojas guardan un símbolo; los nodos internos no. */
typedef struct NodoHuffman {
    unsigned char simbolo;      /* Byte que representa (solo válido en hojas) */
    uint32_t frecuencia;        /* Cuántas veces aparece (o suma de los hijos) */
    struct NodoHuffman *izq;    /* Hijo izquierdo  -> bit 0 */
    struct NodoHuffman *der;    /* Hijo derecho    -> bit 1 */
} NodoHuffman;

/* Código de un símbolo guardado como texto, por ejemplo "0110" */
typedef struct {
    char codigo[MAX_LONGITUD_CODIGO];
    int longitud;
} CodigoHuffman;

/* Tabla de códigos: tabla['a'] contiene el código del símbolo 'a' */
typedef struct {
    CodigoHuffman tabla[MAX_SIMBOLOS];
    bool presente[MAX_SIMBOLOS];  /* true si el símbolo aparece en el archivo */
} Diccionario;

/* Funciones del algoritmo (implementadas en cat_huffman.c) */
unsigned char* leer_archivo(const char *ruta_archivo, size_t *tamano_salida);
void contar_frecuencias(const unsigned char *datos, size_t tamano_datos,
                        uint32_t frecuencias[MAX_SIMBOLOS]);
NodoHuffman* crear_arbol_huffman(const uint32_t frecuencias[MAX_SIMBOLOS]);
void generar_codigos(NodoHuffman *raiz, Diccionario *diccionario);
int guardar_comprimido(const char *ruta_salida, const unsigned char *datos_originales,
                       size_t tamano_original, const uint32_t frecuencias[MAX_SIMBOLOS],
                       const Diccionario *diccionario);
int descomprimir_archivo(const char *ruta_comprimido, const char *ruta_descomprimido);
void liberar_arbol(NodoHuffman *raiz);
void asegurar_directorio_padre(const char *ruta_archivo);
void registrar_archivo_salida(const char *ruta_registro, const char *operacion,
                              const char *ruta_origen, const char *ruta_destino,
                              size_t tamano_origen, long tamano_destino,
                              double porcentaje_ahorro);

/* Progreso y tarea en segundo plano (usados por el editor) */
void huffman_poner_progreso(int porcentaje);  /* escribe el % (omp atomic) */
int huffman_leer_progreso(void);              /* lee el % (omp atomic) */
int huffman_esta_ocupado(void);               /* 1 si hay una tarea en curso */
int huffman_en_segundo_plano(const char *modo, const char *archivo); /* omp task */
void quitar_extension(const char *ruta, char *salida, size_t tamano_salida);

/* Comando del shell: huffman comp <archivo> | huffman desc <archivo.huf> [salida] */
int cmd_huffman(int argc, char **argv);   /* Syscalls: open, read, write, lseek, stat, close */

#endif /* SHELL_H */
