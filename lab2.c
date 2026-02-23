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

// extern char hidtoascii(uint8_t keycode, uint8_t modifiers); // THIS IS A STUB FOR MY OWN TESTING ON MY LAPTOP. FUNCTION DOES NOT EXIST: TODO IMPLIMENT!

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
void reset_rows(int row, int count);
void init_frame_buffer();
int send_message(char* buffer, int buffer_len);

int main()
{
  int err, col, row;

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
  } else {
    // Initializes Frame Buffer
    init_frame_buffer();
  }

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
        // char ch = hidtoascii(packet.keycode[0], packet.modifiers); // TODO: NEED TO IMPLEMENT THIS FUNCTION! THIS IS A STUB FOR TESTING OTHER FUNCTIONS ON MY MAC.

        // Send to Socket if "Enter/Return" pressed.
        if (packet.keycode[0] == 0x28) {
          send_len = send_message(send_buffer, send_len);
        } else if (ch && send_len + 1 < BUFFER_SIZE) { 
          // Add char to buffer if it exists and the buffer is not full.
          send_buffer[send_len++] = ch;

          // Get row and col from buffer len to wrap text to next line.
          row = FB_ROWS - 3 + ((send_len - 1) / FB_COLS);
          col = (send_len - 1) % FB_COLS;

          // MUTEX THREAD Gaurd 
          pthread_mutex_lock(&fb_mutex);
          fbputchar(ch, row, col);
          pthread_mutex_unlock(&fb_mutex);
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
  int row = 8;

  /* Receive data */
  while ( (n = read(sockfd, &recvBuf, BUFFER_SIZE - 1)) > 0 ) {
    recvBuf[n] = '\0';
    printf("%s", recvBuf);

    // Determine how many rows to output read buffer.
    int row_count = (n + FB_COLS - 1) / FB_COLS;

   for (int i = 0; i < row_count; i++) {
      // Reset row before writing to it.
      reset_rows(row + i, 1);
      // MUTEX THREAD Guard
      pthread_mutex_lock(&fb_mutex);
      fbputs(&recvBuf[i * FB_COLS], row + i, 0); // Only write fraction of buffer that fits on the current row.
      pthread_mutex_unlock(&fb_mutex);
   }
   row += row_count;  // advance past the rows we just used in our last write.

   if (row >= FB_ROWS - 4)
       row = 8;  // wrap back to top of message area (FB_ROWS - 4 is the divide between the receipt and send regions / Row 8 is the top).
  }

  return NULL;
}


void reset_rows(int row, int count) {
  // Reset number of rows starting at `row`.
  pthread_mutex_lock(&fb_mutex);
  for (int i = 0; i < count; i++) {
    for (int col = 0; col < FB_COLS; col++) {
      fbputchar(' ', row + i, col);
    }
  }
  pthread_mutex_unlock(&fb_mutex);
}


void init_frame_buffer() {
  int row, col;
  /* Reset Screen/Frame Buffer */
  for (col = 0; col < FB_COLS; col++) {
    for (row = 0; row < FB_ROWS; row++) {
      fbputchar(' ', row, col);
    }
  }

  /* Draw rows of asterisks across the top and bottom of the screen */
  for (col = 0 ; col < 64 ; col++) {
    fbputchar('*', 0, col);
    fbputchar('*', FB_ROWS - 1, col);
  }

  /* Split the Screen into two parts w/ line 2 rows above bottom border. */
  for (col = 0 ; col < FB_COLS; col += 2) {
    fbputchar('-', FB_ROWS - 4, col);
  }

  // Prints message to frame buffer at row 4, column 10.
  fbputs("Hello CSEE 4840 World!", 4, 10);
}

int send_message(char* buffer, int buffer_len) {
  // Transforms to C-String.
  buffer[buffer_len++] = '\n';
  buffer[buffer_len]   = '\0';

  // Send data to socket.
  ssize_t sent = write(sockfd, buffer, buffer_len);

  // Error Handeling
  if (sent < 0)
    printf("Message Send Failed. Error: %zd\n", sent);

  // Reset Buffer
  int reset_cnt = (buffer_len + FB_COLS - 1) / FB_COLS;
  reset_rows(FB_ROWS - 3, reset_cnt);

  return 0;
}

// =========== NOTES ===========
// The shared resouce is the frame buffer and the socket.
// User needs to access frame buffer and write socket.
// Network needs to access read socket and frame buffer.

// Threading is done.
// Keyboard Mapping needs to be implmented. I used an LLM to make a fake simulation with std C I/O just for testing my portion of the lab.
// Frame buffer is half done.
// I made it:
//  1. Clear the screen when the program starts
//  2. Split the screen into two parts with a horizontal line. Have the user enter text on the bottom two rows; use the rest to record what s/he and other users send.
//  3. When a packet arrives, print its contents in the “receive” region. Don’t forget to wrap long messages across multiple lines.
//  4. When printing reaches the bottom of the area, you may either start again at the top, or scroll the entry region of the screen. (Mine starts from the top but does not clear).
// STILL TO DO:
// – Implement a reasonable text-editing system for the bottom of the screen. Have
// input from the keyboard display characters there and allow users to erase
// unwanted characters and send the message with return. Clear the bottom area
// when a message is sent.
// – Display a cursor where the user is typing. This could be a vertical line, an
// underline, or a white box.