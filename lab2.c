/*
 *
 * CSEE 4840 Lab 2 for 2019
 *
 * By: Nicola Paparella, Connor Marvin, Christian Scaff
 * Uni: cm4662, np2953, cts2148
 */
#include "fbputchar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "usbkeyboard.h"
#include <pthread.h>

extern char hidtoascii(uint8_t keycode, uint8_t modifiers);

/* Update SERVER_HOST to be the IP address of
 * the chat server you are connecting to
 */
/* arthur.cs.columbia.edu */
#define SERVER_HOST "128.59.19.114"
#define SERVER_PORT 42000

#define BUFFER_SIZE 128

// Frame Buffer Lock
pthread_mutex_t fb_mutex;

/*
 * References:
 *
 * https://web.archive.org/web/20130307100215/http://beej.us/guide/bgnet/output/html/singlepage/bgnet.html
 *
 * http://www.thegeekstuff.com/2011/12/c-socket-programming/
 * 
 */

int sockfd; /* Socket file descriptor */

// Represents open USB device connection.
struct libusb_device_handle *keyboard;
// Address of specific USB endpoint.
uint8_t endpoint_address;

pthread_t network_thread;
void *network_thread_f(void *);
void access_frame_buffer(const char *s, int row, int col);

int main()
{
  int err, col;

  struct sockaddr_in serv_addr;

  struct usb_keyboard_packet packet;
  int transferred;
  char keystate[12];

  // Internal Buffer
  char send_buffer[BUFFER_SIZE];
  int send_len = 0;

  // Opens Frame Buffer
  pthread_mutex_init(&fb_mutex, NULL);
  if ((err = fbopen()) != 0) {
    fprintf(stderr, "Error: Could not open framebuffer: %d\n", err);
    exit(1);
  }

  /* Draw rows of asterisks across the top and bottom of the screen */
  for (col = 0 ; col < 64 ; col++) {
    fbputchar('*', 0, col);
    fbputchar('*', 23, col);
  }

  // Prints message to frame buffer at row 4, column 10.
  fbputs("Hello CSEE 4840 World!", 4, 10);

  /* Open the keyboard */
  if ( (keyboard = openkeyboard(&endpoint_address)) == NULL ) {
    fprintf(stderr, "Did not find a keyboard\n");
    exit(1);
  }
    
  /* Create a TCP communications socket */
  // AF_INET = IPv4 protocol.
  // SOCK_STREAM = TCP Style Socket Type.
  // 0 = Default protocol for AF_INET + SOCK_STREAM.
  if ( (sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) {
    fprintf(stderr, "Error: Could not create socket\n");
    exit(1);
  }

  /* Get the server address */
  // Zeroes out serv_addr memory location to avoid garbage memory.
  memset(&serv_addr, 0, sizeof(serv_addr));

  // Specifies address type.
  serv_addr.sin_family = AF_INET;

  // Sets port w/ host to network function.
  serv_addr.sin_port = htons(SERVER_PORT);

  // Converts IP address to binary string.
  if ( inet_pton(AF_INET, SERVER_HOST, &serv_addr.sin_addr) <= 0) {
    fprintf(stderr, "Error: Could not convert host IP \"%s\"\n", SERVER_HOST);
    exit(1);
  }

  /* Connect the socket to the server */
  if ( connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
    fprintf(stderr, "Error: connect() failed.  Is the server running?\n");
    exit(1);
  }

  /* Start the network thread */
  pthread_create(&network_thread, NULL, network_thread_f, NULL);

  /* Look for and handle keypresses */
  if (keyboard != NULL) {
    for (;;) {
      // Keyboard USB interrupt signal (8 Byte Packet [Modifier Key (1 B) | Empty (1 B) | Key_Codes (6 B)])
      // Blocking Operation
      libusb_interrupt_transfer(keyboard, endpoint_address,
			      (unsigned char *) &packet, sizeof(packet),
			      &transferred, 0);
      if (transferred == sizeof(packet)) {
        sprintf(keystate, "%02x %02x %02x", packet.modifiers, packet.keycode[0],
	        packet.keycode[1]);
        printf("%s\n", keystate);

        // Temp HID to ASCII:
        char ch = 'a'; // TODO: Implement a 'hidtoascii(packet.keycode[0], packet.modifiers);'

        // Send to Socket if "Enter/Return" pressed.
        if (packet.keycode[0] == 0x28) {
          send_buffer[send_len++] = '\n';
          send_buffer[send_len]   = '\0'; // Transforms to C-String.
          ssize_t sent = write(sockfd, send_buffer, send_len);
          if (sent < 0)
            printf("Message Send Failed. Error: %zd\n", sent);
          send_len = 0; // Reset Buffer
        // Add char to send buffer if it exists and the buffer is not full.
        } else if (ch && send_len + 1 < BUFFER_SIZE) { 
          send_buffer[send_len++] = ch;
          // Convert to C-String
          char tmp[2] = {ch, '\0'};
          access_frame_buffer(tmp, 6, send_len - 1);
        }
        if (packet.keycode[0] == 0x29) { /* ESC pressed? */
	        break;
        }
      }
    }
  }

  /* Terminate the network thread */
  pthread_cancel(network_thread);

  /* Wait for the network thread to finish */
  pthread_join(network_thread, NULL);

  return 0;
}

void *network_thread_f(void *ignored)
{
  /* network_thread_f
     Receives data from the network, leaving the main program thread
     to handle the USB keyboard. */
  
  char recvBuf[BUFFER_SIZE];
  int n;
  /* Receive data */
  while ( (n = read(sockfd, &recvBuf, BUFFER_SIZE - 1)) > 0 ) {
    recvBuf[n] = '\0';
    printf("%s", recvBuf);
    // Frame Buffer Access From Network
    access_frame_buffer(recvBuf, 8, 0);
  }

  return NULL;
}

void access_frame_buffer(const char *s, int row, int col) {
  pthread_mutex_lock(&fb_mutex);
  fbputs(s, row, col);
  pthread_mutex_unlock(&fb_mutex);
}

// =========== NOTES ===========
// The shared resouce is the frame buffer and the socket.
// User needs to access frame buffer and socket.
// Network needs to access socket and frame buffer.