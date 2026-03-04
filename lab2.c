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


#define FB_ROWS 24
#define FB_COLS 64

/* Update SERVER_HOST to be the IP address of
 * the chat server you are connecting to
 */
/* arthur.cs.columbia.edu */
#define SERVER_HOST "128.59.19.114"
#define SERVER_PORT 42000

#define BUFFER_SIZE 128

/* Common HID keycodes (USB HID Usage ID for keyboard) */
#define HID_KEY_ESC        0x29
#define HID_KEY_ENTER      0x28
#define HID_KEY_BACKSPACE  0x2a
#define HID_KEY_SPACE      0x2c

#define HID_KEY_RIGHT      0x4f
#define HID_KEY_LEFT       0x50
#define HID_KEY_DOWN       0x51
#define HID_KEY_UP         0x52

/* row 21, just below dashed divider */
#define INPUT_START_ROW (FB_ROWS - 3)  
/* rows 21 and 22 available for input */
#define INPUT_MAX_ROWS 2               

/* Keep an input buffer for what the user is typing */
static char input_line[BUFFER_SIZE];
static int  input_len = 0;
static int  cursor_pos = 0;

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

// ================== FRAME BUFFER ==================

void reset_rows(int row, int count) {
  // Reset number of rows starting at `row`.
  for (int i = 0; i < count; i++) {
    for (int col = 0; col < FB_COLS; col++) {
      fbputchar(' ', row + i, col);
    }
  }
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

/* Render the current input line and a simple cursor.
 * Cursor is drawn as an underscore '_' at the cursor position.
 * Mutex-protected since network thread also writes to the framebuffer.
 */
static void render_input_line(void)
{
  int i, row, col;

  // GUARD FRAME BUFFER
  pthread_mutex_lock(&fb_mutex);

  /* Clear both input rows */
  for ( row = 0; row < INPUT_MAX_ROWS; row++)
    for (col = 0; col < FB_COLS; col++)
      fbputchar(' ', INPUT_START_ROW + row, col);

  /* Draw text, wrapping at FB_COLS. Taking Christian's row word wrap. */
  for (i = 0; i < input_len; i++) {
    row = INPUT_START_ROW + i / FB_COLS;
    col = i % FB_COLS;
    if (row >= INPUT_START_ROW + INPUT_MAX_ROWS) break;
    fbputchar(input_line[i], row, col);
  }

  /* Draw cursor w/ word wrapping too. */
  if (cursor_pos >= 0 && cursor_pos <= input_len) {
    row = INPUT_START_ROW + cursor_pos / FB_COLS;
    col = cursor_pos % FB_COLS;
    if (row < INPUT_START_ROW + INPUT_MAX_ROWS)
      fbputchar('_', row, col);
  }

  // GUARD FRAME BUFFER
  pthread_mutex_unlock(&fb_mutex);
}

// ================== NETWORKING ==================

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
      // MUTEX THREAD Guard
      pthread_mutex_lock(&fb_mutex);
      // Reset row before writing to it.
      reset_rows(row + i, 1);
      fbputs(&recvBuf[i * FB_COLS], row + i, 0); // Only write fraction of buffer that fits on the current row.
      pthread_mutex_unlock(&fb_mutex);
   }
   row += row_count;  // advance past the rows we just used in our last write.

   if (row >= FB_ROWS - 4)
      // Rest entire input feed.
      pthread_mutex_lock(&fb_mutex);
      reset_rows(8, (INPUT_START_ROW - 10));
      pthread_mutex_unlock(&fb_mutex);
      row = 8;  // wrap back to top of message area (FB_ROWS - 4 is the divide between the receipt and send regions / Row 8 is the top).
  }

  return NULL;
}

/* Send the current input line to the server (followed by newline),
 * then clear the input line.
 */
static void input_send_and_clear(void)
{
  if (sockfd >= 0) {
    /* Send exactly what was typed w/ newline appended */
    input_line[input_len] = ' ';
    if (input_len > 0) {
      ssize_t sent = write(sockfd, input_line, input_len + 1);
      if (sent < 0) {
        printf("Message Send Failed. Error: %zd\n", sent);
      }
    }
  }

  /* Clear the input buffer */
  input_len = 0;
  cursor_pos = 0;
  input_line[0] = '\0';
}

// ================== USB KEYBOARD ==================

/* Return 1 if either shift key is currently held down */
static int is_shift_down(uint8_t mods)
{
  /* USB_LSHIFT and USB_RSHIFT come from usbkeyboard.h */
  if ( (mods & USB_LSHIFT) != 0 ) return 1;
  if ( (mods & USB_RSHIFT) != 0 ) return 1;
  return 0;
}

/* Check whether keycode kc exists inside the 6-byte keycode array */
static int keycode_in_packet(uint8_t kc, const struct usb_keyboard_packet *p)
{
  int i;
  for (i = 0; i < 6; i++) {
    if (p->keycode[i] == kc) {
      return 1;
    }
  }
  return 0;
}

/* Detect "new press":
 * - USB reports keep repeating while a key is held.
 * - We only want to act once per press (unless you implement autorepeat later).
 *
 * New press definition:
 *   key is in current packet, but NOT in previous packet.
 */
static int is_new_keypress(uint8_t kc,
                           const struct usb_keyboard_packet *cur,
                           const struct usb_keyboard_packet *prev)
{
  if (kc == 0) return 0; /* 0 means "no key" */
  if (keycode_in_packet(kc, cur) && !keycode_in_packet(kc, prev)) {
    return 1;
  }
  return 0;
}


static char hid_keycode_to_ascii(uint8_t kc, uint8_t mods)
{
  int shift = is_shift_down(mods);

  /* Letters a-z are HID 0x04..0x1d */
  if (kc >= 0x04 && kc <= 0x1d) {
    char base = (char)('a' + (kc - 0x04));
    if (shift) {
      base = (char)('A' + (kc - 0x04));
    }
    return base;
  }

  /* Numbers 1..0 are HID 0x1e..0x27 */
  if (kc >= 0x1e && kc <= 0x27) {
    /* unshifted: 1 2 3 4 5 6 7 8 9 0
     * shifted:   ! @ # $ % ^ & * ( )
     */
    static const char unshifted_nums[] = "1234567890";
    static const char shifted_nums[]   = "!@#$%^&*()";
    int idx = (int)(kc - 0x1e);
    if (shift) return shifted_nums[idx];
    return unshifted_nums[idx];
  }

  /* Enter, Backspace, Space */
  if (kc == HID_KEY_ENTER)     return '\n';
  if (kc == HID_KEY_BACKSPACE) return '\b';
  if (kc == HID_KEY_SPACE)     return ' ';

  /* Punctuation keys (US layout) */
  switch (kc) {
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default: break;
  }

  return 0;
}


/* Insert a character at the cursor position inside input_line */
static void input_insert_char(char ch)
{
  int i;

  /* Don't overflow the buffer */
  if (input_len >= BUFFER_SIZE - 1) {
    return;
  }

  /* Move everything to the right to make space */
  for (i = input_len; i > cursor_pos; i--) {
    input_line[i] = input_line[i - 1];
  }

  input_line[cursor_pos] = ch;
  input_len++;
  cursor_pos++;

  /* Keep the string null-terminated */
  input_line[input_len] = '\0';
}

/* Backspace behavior:
 * delete character to the LEFT of cursor, if any
 */
static void input_backspace(void)
{
  int i;

  if (cursor_pos <= 0) {
    return;
  }

  /* Shift everything left starting from cursor_pos-1 */
  for (i = cursor_pos - 1; i < input_len - 1; i++) {
    input_line[i] = input_line[i + 1];
  }

  input_len--;
  cursor_pos--;

  input_line[input_len] = '\0';
}

/* Move cursor left/right by 1 */
static void input_cursor_left(void)
{
  if (cursor_pos > 0) cursor_pos--;
}
static void input_cursor_right(void)
{
  if (cursor_pos < input_len) cursor_pos++;
}

int main()
{
  int err;
  char keystate[12];

  struct sockaddr_in serv_addr;

  struct usb_keyboard_packet packet;
  struct usb_keyboard_packet prev_packet;
  int transferred;

  /* Initialize the input buffer */
  input_len = 0;
  cursor_pos = 0;
  input_line[0] = '\0';
  memset(&prev_packet, 0, sizeof(prev_packet));

  // Opens Frame Buffer
  pthread_mutex_init(&fb_mutex, NULL);
  if ((err = fbopen()) != 0) {
    fprintf(stderr, "Error: Could not open framebuffer: %d\n", err);
    exit(1);
  } else {
    // Initializes Frame Buffer
    init_frame_buffer();
    render_input_line();
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
      libusb_interrupt_transfer(keyboard, endpoint_address,
			      (unsigned char *) &packet, sizeof(packet),
			      &transferred, 0);
      if (transferred == sizeof(packet)) {
        sprintf(keystate, "%02x %02x %02x", packet.modifiers, packet.keycode[0],
        packet.keycode[1]);
        printf("%s\n", keystate);
        if (packet.keycode[0] == 0x29) { /* ESC pressed? */
	  break;
        }

        /* If ESC is in ANY slot (not just keycode[0]), exit */
        if (keycode_in_packet(HID_KEY_ESC, &packet)) {
          break;
        }

        /* Walk through all 6 possible key slots. */
        {
          int i;
          for (i = 0; i < 6; i++) {
            uint8_t kc = packet.keycode[i];

            /* Only act on "new press" (otherwise a held key repeats constantly) */
            if (!is_new_keypress(kc, &packet, &prev_packet)) {
              continue;
            }

            /* Handle arrow keys first (not ASCII) */
            if (kc == HID_KEY_LEFT) {
              input_cursor_left();
              render_input_line();
              continue;
            }
            if (kc == HID_KEY_RIGHT) {
              input_cursor_right();
              render_input_line();
              continue;
            }
            if (kc == HID_KEY_UP) {
              /* Not required for the demo; just ignore for now */
              continue;
            }
            if (kc == HID_KEY_DOWN) {
              /* Not required for the demo; just ignore for now */
              continue;
            }

            /* Convert to ASCII for printable keys */
            {
              char ch = hid_keycode_to_ascii(kc, packet.modifiers);

              /* If 0, it means we don't handle that key (ignore it) */
              if (ch == 0) {
                continue;
              }

              /* Special ASCII-like actions */
              if (ch == '\b') {
                /* Backspace */
                input_backspace();
                render_input_line();
                continue;
              }

              if (ch == '\n') {
                /* Enter: send typed line over network, then clear */
                input_send_and_clear();
                render_input_line();
                continue;
              }

              /* Otherwise it is a normal printable char */
              input_insert_char(ch);
              render_input_line();
            }
          }
        }

        /* Save current packet as previous packet for next iteration */
        prev_packet = packet;
      }
    }
  }

  /* Terminate the network thread */
  pthread_cancel(network_thread);

  /* Wait for the network thread to finish */
  pthread_join(network_thread, NULL);

  return 0;
}