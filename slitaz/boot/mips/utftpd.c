/*****************************************************************************
* utftpd.c - A tiny TFTP program
* By Martijn van Oosterhout <kleptog@cupid.suninternet.com> Copyright (C) 2000
*
* This program is released under the GNU General Public Licence.
*
* Please retain all references to me (Martijn) in this file.
*
* Original source: http://cupid.suninternet.com/~kleptog/tftp/
* See that site for documentation.
******************************************************************************/

/* 2003/10/03 Mark Verboom <m.verboom@corp.home.nl>
 *
 * - Fixed to else clauses to prevent segfaults
 * - Fixed netmask option commandline parsing
 * - Added -d deamon mode option
 * - Added --help option
 * - Changed behaviour of paths on commandline as search path
 * - Changed all errors to syslog entries
 * - Added signal handler to display statistics in syslog
 * - Changed layout of source to tab indented
 * - Changed makefile for Solaris to compile 64bit to allow for more than
 *   256 filedescriptors
 * - Added option to set max filediscriptors to max
 * - Changed log facility to LOCAL0 to be able to split logfiles
 */
 
#define VERSION "0.2ads"
#define BLOCK_SIZE 512
#define MIN_BLOCK_SIZE 8
#define MAX_BLOCK_SIZE 65564 /* RFC2348 says between 8 and 65564 */
#define MAX_CONNECTS 8192
#define FD_SETSIZE 8192
#define TIMEOUT 1
#define RESEND_LIMIT 10
#define MAX_ADDRS 16
#define MAX_PATHS 16

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <string.h>
#include <strings.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>

enum { PKT_RRQ=1, PKT_WRQ, PKT_DATA, PKT_ACK, PKT_ERROR, PKT_OACK } PacketTypes;
enum { ERR_UNKNOWN = 0, ERR_NOTFOUND, ERR_PERM, ERR_DISKFULL, ERR_ILLEGAL, ERR_BADTID, ERR_EXIST, ERR_USER } ErrorTypes;

struct Connection
{
  int sock;
  struct sockaddr_in addr;
  FILE *file;
  int block;
  struct timeval lasttime;
  int resend;
  int blksize;
};

struct Statistics
{
  long  timeout;
  long  retries;
  long  sentbytes;
  long  sentpackets;
} stats;

fd_set ports;

struct Connection *Connections[MAX_CONNECTS];

char *ValidPaths[MAX_PATHS];
int NumPaths;
int Logging;
int Daemon = 0;
int Ris = 0;
int blksize;

struct { struct in_addr addr; int mask; } ValidAddresses[MAX_ADDRS];
int NumAddresses;

void Log( char *str, ... )
{
  va_list v;
  char buffer[512];
  va_start(v, str);

  vsnprintf( buffer, 511, str, v );
  
  syslog( LOG_INFO, "%s", buffer );
}

void CloseConnection( int i )
{
  fclose( Connections[i]->file );
  close( Connections[i]->sock );
  FD_CLR( Connections[i]->sock, &ports );
  free( Connections[i] );
  Connections[i] = NULL;
}

void SendOAck( int fd, struct sockaddr_in *source, char *str, int len )
{
  struct {
    short type;
    char msg[BLOCK_SIZE];
  } buf;

  buf.type = htons(PKT_OACK);
  memcpy( buf.msg, str, len );

  if( source )
    sendto( fd, &buf, len+2, 0, (struct sockaddr *) source, 
	    sizeof(struct sockaddr_in) );
  else
    write( fd, &buf, len+2 );
}

void SendError( int fd, struct sockaddr_in *source, int errtype, char *str )
{
  struct {
    short type;
    short errnum;
    char msg[BLOCK_SIZE];
  } buf;

  buf.errnum = htons(errtype);
  buf.type = htons(PKT_ERROR);
  strcpy( buf.msg, str );

  if( source )
    sendto( fd, &buf, strlen(str)+5, 0, (struct sockaddr *) source, 
	    sizeof(struct sockaddr_in) );
  else
    write( fd, &buf, strlen(str)+5 );
}

void SendBlock( int i )
{
  struct {
    short type;
    short blocknum;
    char buffer[MAX_BLOCK_SIZE];
  } buf;
  int len;
  struct Connection *conn = Connections[i];

  if( feof( conn->file ) && ftell(conn->file) < conn->block*conn->blksize )
  {
    CloseConnection( i );
    return;
  }

  fseek( conn->file, (conn->block-1)*conn->blksize, SEEK_SET );

  len = fread( buf.buffer, 1, conn->blksize, conn->file );

  buf.type = htons(PKT_DATA);
  buf.blocknum = htons(conn->block);

  write( conn->sock, &buf, len+4 );

  stats.sentbytes += len;
  stats.sentpackets++;
}

void showhelp(void)
{
  fprintf(stderr, "Usage: utftpd [OPTION]... [NETWORK]... [DIRNAME]...\n");
  fprintf(stderr, "Serve files through tftp\n\n");
  fprintf(stderr, "  -d                run as daemon\n");
  fprintf(stderr, "  -r                remote installation service support\n");
  fprintf(stderr, "  -l                log through syslog\n");
  fprintf(stderr, "  -h, --help        display this help and exit\n");
  fprintf(stderr, "  -v, --version     display version and exit\n");
  fprintf(stderr, "  NETWORK           network which is allowed to request files, example 10.40.10.0/24 (max. %d)\n", MAX_ADDRS);
  fprintf(stderr, "  DIRNAME           directory from which files can be served (max. %d)\n", MAX_PATHS);

  exit(1);
}

int CheckArguments( int argc, char *argv[] )
{
  for( argc--, argv++; argc; argc--, argv++ )
  {
    int ok = 0;

    if (*argv[0] == '-') {
	char *s = argv[0];
	if (*++s == '-') s++;
	switch (*s) {
	case 'l' : Logging = 1; continue;
	case 'd' : Daemon = 1; continue;
	case 'r' : Ris = 1; continue;
	case 'v' : fprintf(stderr, "utftpd %s\n", VERSION); break;
	default  : showhelp();
	}
    }

    if( argv[0][0] == '/' ) // Pathname
    {
      if( NumPaths > MAX_PATHS ) {
        Log("Too many paths");
        return 1;
      }
      ValidPaths[ NumPaths++ ] = strdup(argv[0]);
      continue;
    }

    do {
      int mask;
      struct in_addr addr;
      char *end;
      char *ptr = strchr( argv[0], '/' );
 
      if( ptr ) *ptr = 0;
      if( !inet_aton( argv[0], &addr ) )
      {
        if( ptr ) *ptr = '/';
          break;
      }

      if( ptr )
      {
        mask = strtol( ptr+1, &end, 10 );
        if( *end ) break;
      }
      else
        mask = 0;

      if( NumAddresses > MAX_ADDRS )
      {
        Log("Too many addresses");
        return 1;
      }
      ValidAddresses[ NumAddresses ].addr = addr;
      ValidAddresses[ NumAddresses ].mask = htonl((~0)<<(32 - mask));
      NumAddresses++;
      ok = 1;
    } while(0);

    if( !ok )
    {
      Log("Bad option");
      return 1;
    }
  }
  return 0;
}

// lookup search and concat real filename to unixpath
static int lookup_entry(const char *search, char *unixpath)
{
	int status = 0;
	DIR *dirp = opendir(unixpath[0] ? unixpath : ".");

	if (dirp != NULL) {
		struct dirent *entry;

		while ((entry = readdir(dirp))) {
			if (!strcasecmp(entry->d_name, search)) {
				if (unixpath[0]) strcat(unixpath, "/");
				strcat(unixpath, entry->d_name);
				status++;
				break;
			}
		}
		closedir(dirp);
	}
	return status;
}

FILE *CheckFileOpen( char *filename, struct sockaddr_in *source )
{
  errno = EPERM;
  FILE *fd = NULL;
  char *servfile;
  
  if( NumAddresses )
  {
    int i;

    for( i=0; i<NumAddresses; i++ )
    {
          /*  printf("(%08X ^ %08X) & %08X == %08X\n", ValidAddresses[i].addr.s_addr, source->sin_addr.s_addr, ValidAddresses[i].mask, ((ValidAddresses[i].addr.s_addr ^ source->sin_addr.s_addr) & ValidAddresses[i].mask ) ); */
      if( ((ValidAddresses[i].addr.s_addr ^ source->sin_addr.s_addr) & ValidAddresses[i].mask ) == 0 )
        break;
    }

    if( i == NumAddresses ) return NULL;
  }

  if( NumPaths )
  {
    int i;
    
    if( strstr( filename, "/../" ) )
      return NULL;

    for( i=0; i<NumPaths; i++ )
    {
      servfile = malloc(sizeof(char) * (strlen(ValidPaths[i]) + strlen(filename) + 1));
      if (servfile == NULL)
      {
        if( Logging ) 
          Log("Unable to allocate memory.\n");
        exit(1);
      }
      if (Ris != 0 && filename[0] == '\\') { // RIS support
	char unixpath[PATH_MAX];
	char *s = unixpath + 1;
	char *check = filename + 1;
	int len;

	strncpy(unixpath,  ValidPaths[i], PATH_MAX);
	for (;*check; len++, s += len, check += len) {
		char *seek = strchr(check, '\\');
		if (!seek) { // basename of filename
			if (lookup_entry(check, unixpath))
				strcpy(servfile, unixpath); // found
			break;
		}
		len = seek - check;
		memcpy(s, check, len);
		s[len] = '\0';
		if (!lookup_entry(s, unixpath))
			break; // path mismatch
	}
      }
      else sprintf(servfile, "%s/%s", ValidPaths[i], filename);
      fd = fopen( servfile, "r" );
      free(servfile);
      if (fd != NULL)
        break;
    }
  }
  return fd;
}

void sig_usr1(void)
{
  int  i;
  int  conns = 0;

  for( i=0; i<MAX_CONNECTS; i++ )
    if( Connections[i] )
      conns++;

  Log("Statistics dump");
  Log("Current open connections: %d", conns);
  Log("Sent packets: %ld", stats.sentpackets);
  Log("Retried packets: %ld", stats.retries);
  if (stats.sentpackets > 0)
    Log("Retried %% : %.2f%%", (float) (stats.retries/stats.sentpackets) * 100);
  Log("Timed out connections: %ld", stats.timeout);
  Log("Data sent: %ldKB", stats.sentbytes / 1024);

  if (signal(SIGUSR1, (sig_t)sig_usr1) < 0 )
  {
    if ( Logging )
      Log("signal: %s\n", strerror(errno));
    exit(1);
  }
}

int main( int argc, char *argv[] )
{
  int fd;
  int pid;
  struct rlimit  rlp;

  // Process command-line arguments
  if( CheckArguments( argc, argv ) ) return 1;

  //  printf("%d paths and %d addresses\n", NumPaths, NumAddresses );
  openlog( "utftpd", LOG_PID, LOG_LOCAL0 );

  if (Daemon)
  {
    pid = fork();
    if (pid < 0)
    {
       if( Logging ) 
        Log("Unable to fork child process to run as Daemon.\n");
      exit(1);
    }
    if (pid > 0 )
      exit(0);

    if( Logging ) 
      Log("Running as daemon.\n");
  }

  rlp.rlim_cur = MAX_CONNECTS;
  rlp.rlim_max = MAX_CONNECTS;

  if (setrlimit(RLIMIT_NOFILE, &rlp) != 0)
  {
    perror("setrlimit");
    exit(1);
  }

  if( Logging ) 
  {
    Log("Running from commandline.\n");
    Log("Max open files: %d\n", rlp.rlim_max);
    Log("Max select FD_SETSIZE: %d\n", FD_SETSIZE);
    Log("FOPEN_MAX: %d\n", FOPEN_MAX);
  }

  if (signal(SIGUSR1, (sig_t)sig_usr1) < 0 )
  {
    if ( Logging )
      Log("signal: %s\n", strerror(errno));
     exit(1);
  }

  // Bind server socket
  fd = socket( AF_INET, SOCK_DGRAM, 0 );

  if( fd == -1 ) 
  { 
    if( Logging ) 
      Log("socket: %s", strerror(errno));
    return 1; 
  }

  {
    struct servent *ent = getservbyname("tftp","udp");
    struct sockaddr_in sin;
    
    if( !ent ) 
    { 
      if( Logging ) 
        Log("getservbyname: %s", strerror(errno));
      return 1; 
    }

    bzero( &sin, sizeof(sin) );
    sin.sin_port = ent->s_port;
    sin.sin_family = AF_INET;

    if( bind( fd, (struct sockaddr *) &sin, sizeof(sin) ) != 0 ) 
    { 
      if( Logging ) 
        Log("bind: %s", strerror(errno));
      return 1; 
    }
  }

  {
    int null;
    
    // dup() /dev/null onto standard descriptors
    if( (null = open( "/dev/null", O_RDONLY )) == -1 )
    {
      fprintf( stderr, "FATAL: Couldn't open /dev/null for reading\n" );
      return 0;
    }

    // Close any connection to outside world
    dup2( null, 0 );
    dup2( null, 1 );
    dup2( null, 2 );
    close( null );
  }
  
  {
    fd_set temp;
    struct { short type; char message[1022]; } buffer;
    int maxfd = fd+1;

    FD_ZERO( &ports );
    FD_SET( fd, &ports );

    for(;;)
    {
      struct timeval tv = { TIMEOUT,0 };
      int count;

      // Wait for a packet to come in
      memcpy( &temp, &ports, sizeof(fd_set) );
      count = select( maxfd, &temp, NULL, NULL, &tv );
      if( count == -1 )
      {
        if( errno == EINTR ) 
          continue;
        else
          Log("Error in select"), exit(0);
      }

      // If it's on the original socket, it's a new connection.
      if( FD_ISSET( fd, &temp ) )
      {
        struct sockaddr_in source;
        unsigned sinlen = sizeof( source );
        int len = recvfrom( fd, &buffer, sizeof(buffer), 0,
			    (struct sockaddr *) &source, &sinlen );

        char *filename = buffer.message;
        char *mode = filename;
        int got_blksize = 0, got_tsize = 0;
        
        if( len == -1 )
        {
          Log("Error in recvfrom");
          continue;
        }

        // If it's not a read request, ignore it.
        if( buffer.type != htons(PKT_RRQ) )
          continue;

	blksize = BLOCK_SIZE;
	for (mode = filename; len > 2; len--) {
		if (*mode++) continue;
		if (!strncasecmp(mode,"blksize",8)) {
			got_blksize = 1;
			for (blksize = 0, mode += 8; *mode; )
				blksize = (blksize * 10) + (*mode++ - '0');
		}
		if (!strncasecmp(mode,"tsize",6)) {
			got_tsize = 1;
			mode += 6;
		}
	}

        // Add new connection to list of open connections
        {
          int i, sock, oacksz;
          FILE *file;
          char info[80];

          for( i=0; i<MAX_CONNECTS; i++ )
            if( !Connections[i] )
              break;

          if( i == MAX_CONNECTS )
          {
            Log("Maximum number of connections reached\n");
            SendError( fd, &source, ERR_UNKNOWN, "Maximum number of connections reached" );
            continue;
          }

          if( blksize < MIN_BLOCK_SIZE || blksize > MAX_BLOCK_SIZE )
          {
	    sprintf(info,"Bad block size %d \n",blksize);
            SendError( fd, &source, ERR_ILLEGAL, info );
            continue;
          }

          // Open file if permissions allow and it exists
          file = CheckFileOpen( filename, &source );
          if( !file )
          {
            Log("REJECTED sending %s to %s (%d open connections, error %s)\n", filename, inet_ntoa( source.sin_addr ), i, strerror(errno));
            SendError( fd, &source, (errno==EACCES)?ERR_PERM:ERR_NOTFOUND, strerror(errno) );
            continue;
          }

          if( Logging ) 
            Log("Sending %s to %s\n", filename, inet_ntoa( source.sin_addr ) );
          sock = socket( AF_INET, SOCK_DGRAM, 0 );
          if( sock == -1 )
          {
            SendError( fd, &source, ERR_UNKNOWN, strerror(errno) );
            fclose(file);
            continue;
          }

          connect( sock, (struct sockaddr *) &source, sizeof(source) );
          FD_SET( sock, &ports );
          if( sock+1 > maxfd ) maxfd = sock+1;

          Connections[i] = malloc( sizeof(struct Connection) );
          memcpy( &Connections[i]->addr, &source, sizeof(source) );
          Connections[i]->file = file;
          Connections[i]->block = 1;
          Connections[i]->sock = sock;
          gettimeofday( &Connections[i]->lasttime, NULL );
          Connections[i]->lasttime.tv_sec += TIMEOUT;
          Connections[i]->resend = 0;
          Connections[i]->blksize = blksize;
          oacksz = 0;
          if (got_tsize) {
              struct stat st;
              
              fstat(fileno(file),&st);
              oacksz += sprintf(info + oacksz,"tsize%c%u%c",
                                '\0',(unsigned) st.st_size,'\0');
          }
          if (got_blksize) {
              oacksz += sprintf(info + oacksz,"blksize%c%d%c",
                                '\0',blksize,'\0');
          }
          if (oacksz) SendOAck( sock, &source, info, oacksz);
          else SendBlock( i );
        }
      }
	//else // If it's not on the original socket, it's a old connection.
      {
        int i;
        struct timeval tv;
        gettimeofday( &tv, NULL );
        // Check each open connection
        for( i=0; i<MAX_CONNECTS; i++ )
        {
          if( !Connections[i] ) continue;
          if( FD_ISSET( Connections[i]->sock, &temp ) )
          {
            // We only accept ACKS, if it's not, drop the connection
            int len = read( Connections[i]->sock, &buffer, sizeof(buffer) );
            if( len != 4 || buffer.type != htons(PKT_ACK))
            {
              SendError( Connections[i]->sock, NULL, ERR_ILLEGAL, "Malformed ACK\n");
              CloseConnection( i );
            }
            else
            {
              // Send the requested block
              Connections[i]->block = htons(*((short*)buffer.message))+1;
             
              memcpy( &Connections[i]->lasttime, &tv, sizeof(tv) );
              Connections[i]->lasttime.tv_sec += TIMEOUT;
              Connections[i]->resend = 0;
              SendBlock( i );
            }
          }
          else
          {
            // If the connection has reached timeout stage, resend
            if( timercmp( &Connections[i]->lasttime, &tv, < ) )
            {
              if( Connections[i]->resend > RESEND_LIMIT )
              {
                Log("%s: Too many timeouts, connection closed\n", inet_ntoa( Connections[i]->addr.sin_addr ) );
                stats.timeout++;
                SendError( Connections[i]->sock, NULL, ERR_UNKNOWN, "Timeout\n");
                CloseConnection( i );
              }
              else
              {
                memcpy( &Connections[i]->lasttime, &tv, sizeof(tv) );
                Connections[i]->lasttime.tv_sec += TIMEOUT;
                Connections[i]->resend++;
                stats.retries++;
                SendBlock( i );
              }
            }
          }
        }
      }
    }
  }
}
