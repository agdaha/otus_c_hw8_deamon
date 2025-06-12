#define _GNU_SOURCE

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/un.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <syslog.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syslog.h>
#include <confuse.h>
#include <sys/socket.h>
#include <errno.h>

#define DEFAULT_CONFIG_FILE "my_daemon.conf"
#define SYSLOG_IDENT "my_daemon"
#define BUF_SIZE 1024

typedef struct
{
  char file_path[PATH_MAX];
  char socket_path[PATH_MAX];
} Config;

volatile sig_atomic_t is_running = 1;

void read_config(Config *config, const char *config_path);
void daemonize();
void print_usage(int exit_code, char *prog_name);
void handle_signal(int sig);

void run_server(const Config *config);
void handle_request(int client_socket, const char *file_path);

int main(int argc, char **argv)
{
  if (argc < 2 || argc > 4)
  {
    print_usage(EXIT_FAILURE, argv[0]);
  }

  int is_daemonize = 0;

  Config config;
  const char *config_path = DEFAULT_CONFIG_FILE;

  int opt;
  while ((opt = getopt(argc, argv, "c:dh")) != -1)
  {
    switch (opt)
    {
    case 'c':
      config_path = optarg;
      break;
    case 'd':
      is_daemonize = 1;
      break;
    case 'h':
      print_usage(EXIT_SUCCESS, argv[0]);
      break;
    default:
      print_usage(EXIT_FAILURE, argv[0]);
    }
  }

  // Проверка наличия конфигурационного файла
  if (access(config_path, F_OK) == -1)
  {
    fprintf(stderr, "Config file %s does not exist: %s\n", config_path, strerror(errno));
    exit(EXIT_FAILURE);
  }
  // Проверка на доступность конфигурационного файла для чтения
  if (access(config_path, R_OK) == -1)
  {
    fprintf(stderr, "Config file %s is not readable: %s\n", config_path, strerror(errno));
    exit(EXIT_FAILURE);
  }

  // Инициализировать файл журнала
  openlog(SYSLOG_IDENT, LOG_PID | LOG_CONS, LOG_DAEMON);

  // прочитать конфигурационный файл
  read_config(&config, config_path);

  // повесить обработчики сигналов
  signal(SIGTERM, handle_signal);
  signal(SIGINT, handle_signal);

  // демонизировать приложение
  if (is_daemonize)
  {
    daemonize();
  }

  // запустить сервер
  run_server(&config);

  closelog();
  return EXIT_SUCCESS;
}

void print_usage(int exit_code, char *prog_name)
{
  if (exit_code)
  {
    fprintf(stderr, "Usage: %s -c config_file [-d]\n", prog_name);
  }
  else
  {
    fprintf(stdout, "Usage: %s -c config_file [-d]\n", prog_name);
  }
  exit(exit_code);
}
void daemonize()
{
  pid_t pid;
  struct rlimit rl;
  struct sigaction sa;

  // Сбросить маску режима создания файла.
  umask(0);

  // Получить максимально возможный номер дескриптора файла.
  if (getrlimit(RLIMIT_NOFILE, &rl) < 0)
    perror("невозможно получить максимальный номер дескриптора");

  // Стать лидером нового сеанса, чтобы утратить управляющий терминал.
  if ((pid = fork()) < 0)
    perror("ошибка вызова функции fork");
  else if (pid != 0) /* родительский процесс */
    exit(EXIT_SUCCESS);
  setsid();

  // Обеспечить невозможность обретения управляющего терминала в будущем.
  sa.sa_handler = SIG_IGN;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGHUP, &sa, NULL) < 0)
    syslog(LOG_CRIT, "невозможно игнорировать сигнал SIGHUP");

  if ((pid = fork()) < 0)
    syslog(LOG_CRIT, "ошибка вызова функции fork");
  else if (pid != 0) /* родительский процесс */
    exit(EXIT_SUCCESS);

  /*
   * Назначить корневой каталог текущим рабочим каталогом,
   * чтобы впоследствии можно было отмонтировать файловую систему.
   */
  if (chdir("/") < 0)
    syslog(LOG_CRIT, "невозможно сделать текущим рабочим каталогом /");

  // Закрыть все открытые файловые дескрипторы.
  if (rl.rlim_max == RLIM_INFINITY)
    rl.rlim_max = 1024;
  for (rlim_t i = 0; i < rl.rlim_max; i++)
    close(i);

  // Присоединить файловые дескрипторы 0, 1 и 2 к /dev/null.
  int fd0 = open("/dev/null", O_RDWR);
  int fd1 = dup(0);
  int fd2 = dup(0);
  if (fd0 != 0 || fd1 != 1 || fd2 != 2)
    syslog(LOG_CRIT, "ошибочные файловые дескрипторы %d %d %d", fd0, fd1, fd2);
}

void read_config(Config *config, const char *config_path)
{
  // Чтение конфигурационного файла
  cfg_t *cfg;
  static char *file_path = NULL;
  static char *socket_path = NULL;
  cfg_opt_t opts[] = {
      CFG_SIMPLE_STR("file_path", &file_path),
      CFG_SIMPLE_STR("socket_path", &socket_path),
      CFG_END()};
  cfg = cfg_init(opts, 0);
  cfg_parse(cfg, config_path);
  if (cfg_parse(cfg, config_path) == CFG_PARSE_ERROR)
  {
    cfg_free(cfg);
    syslog(LOG_ERR, "Failed to parse config file %s", config_path);
    exit(EXIT_FAILURE);
  }

  // Проверка наличия обязательных параметров
  if (strlen(file_path) == 0)
  {
    syslog(LOG_ERR, "Error: file_path not specified in config");
    cfg_free(cfg);
    exit(EXIT_FAILURE);
  }
  if (strlen(socket_path) == 0)
  {
    syslog(LOG_ERR, "Error: socket_path not specified in config");
    cfg_free(cfg);
    exit(EXIT_FAILURE);
  }

  strncpy(config->file_path, file_path, PATH_MAX);
  strncpy(config->socket_path, socket_path, PATH_MAX);

  cfg_free(cfg);
}

void handle_signal(int sig)
{
  if (sig == SIGINT || sig == SIGTERM)
  {
    is_running = 0;
  }
}

void handle_request(int client_socket, const char *file_path){
  char response[BUF_SIZE];
  struct stat st;

  if (stat(file_path, &st) == -1) {
    snprintf(response, BUF_SIZE, "ERROR: %s\n", strerror(errno));
    syslog(LOG_WARNING, "Failed to get file size for %s: %s", file_path, strerror(errno));
  } else {
    snprintf(response, BUF_SIZE, "%ld\n", st.st_size);
    syslog(LOG_DEBUG, "Sent file size %ld for %s", st.st_size, file_path);
  }

  write(client_socket, response, strlen(response));
  close(client_socket);
}

void run_server(const Config *config) {
  int server_fd, client_fd;
  struct sockaddr_un server_addr, client_addr;
  socklen_t client_len = sizeof(client_addr);

  // Удалить старый сокет если существует
  unlink(config->socket_path);

  // Создать новый сокет
  if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
      syslog(LOG_ERR, "Failed to create socket: %s", strerror(errno));
      exit(EXIT_FAILURE);
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, config->socket_path, sizeof(server_addr.sun_path) - 1);

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
      syslog(LOG_ERR, "Failed to bind socket: %s", strerror(errno));
      exit(EXIT_FAILURE);
  }

  if (listen(server_fd, 5) == -1) {
      syslog(LOG_ERR, "Failed to listen on socket: %s", strerror(errno));
      exit(EXIT_FAILURE);
  }

  chmod(config->socket_path, 0666);

  syslog(LOG_INFO, "Server started, monitoring file: %s", config->file_path);
  syslog(LOG_INFO, "Listening on socket: %s", config->socket_path);

  // Слушаем соединения и обрабатка запросов
  while (is_running) {
      client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
      if (client_fd == -1) {
          if (is_running) {
              syslog(LOG_ERR, "Failed to accept connection: %s", strerror(errno));
          }
          continue;
      }

      handle_request(client_fd, config->file_path);
  }

  close(server_fd);
  unlink(config->socket_path);
  syslog(LOG_INFO, "Server stopped");
}