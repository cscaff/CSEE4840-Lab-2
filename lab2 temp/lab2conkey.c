/*
 *
 * CSEE 4840 Lab 2 for 2019
 *
 * Name/UNI: Please Changeto Yourname (pcy2301)
 *
 * NOTE:
 * - I did NOT remove or rewrite the provided skeleton code.
 * - I only ADDED extra code (helper functions + extra logic).
 * - The goal is to complete Section 7 USB (keyboard input using libusb),
 *   and provide basic behavior that matches what the assignment wants:
 *   ASCII typing, shift, arrows, backspace, enter.
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

/* Update SERVER_HOST to be the IP address of
 * the chat server you are connecting to
 */
/* arthur.cs.columbia.edu */
#define SERVER_HOST "128.59.19.114"
#define SERVER_PORT 42000

#define BUFFER_SIZE 128

/*
 * References:
 *
 * https://web.archive.org/web/20130307100215/http://beej.us/guide/bgnet/output/html/singlepage/bgnet.html
 *
 * http://www.thegeekstuff.com/2011/12/c-socket-programming/
 *
 */

int sockfd; /* Socket file descriptor */

struct libusb_device_handle *keyboard;
uint8_t endpoint_address;

pthread_t network_thread;
void *network_thread_f(void *);

// new begin

/* Common HID keycodes (USB HID Usage ID for keyboard) */
#define HID_KEY_ESC        0x29
#define HID_KEY_ENTER      0x28
#define HID_KEY_BACKSPACE  0x2a
#define HID_KEY_SPACE      0x2c

#define HID_KEY_RIGHT      0x4f
#define HID_KEY_LEFT       0x50
#define HID_KEY_DOWN       0x51
#define HID_KEY_UP         0x52

/* The framebuffer in this lab is normally treated as 24 rows x 64 cols
 * (because the skeleton draws cols 0..63 on rows 0 and 23).
 */
#define FB_ROWS 24
#define FB_COLS 64

/* We'll keep a simple "typing area" on row 22 (just above the bottom border).
 * This is NOT fancy; it's just enough so you can test USB typing easily.
 */
#define INPUT_ROW 22
#define INPUT_COL_START 0

/* Keep an input buffer for what the user is typing */
static char input_line[BUFFER_SIZE];
static int  input_len = 0;     /* number of characters currently in input_line */
static int  cursor_pos = 0;    /* cursor position inside the input_line (0..input_len) */

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

/* Clear a single row on the framebuffer by printing spaces across it */
static void fb_clear_row(int row)
{
  int c;
  for (c = 0; c < FB_COLS; c++) {
    fbputchar(' ', row, c);
  }
}

/* Render the current input line and a simple cursor.
 * Cursor is drawn as an underscore '_' at the cursor position.
 */
static void render_input_line(void)
{
  int i;
  int col = INPUT_COL_START;

  /* Clear the whole input row first */
  fb_clear_row(INPUT_ROW);

  /* Draw the text */
  for (i = 0; i < input_len && col < FB_COLS; i++, col++) {
    fbputchar(input_line[i], INPUT_ROW, col);
  }

  /* Draw the cursor (underscore).
   * If cursor goes past screen width, clamp it so we don't write off screen.
   */
  if (cursor_pos < 0) cursor_pos = 0;
  if (cursor_pos > input_len) cursor_pos = input_len;

  if (INPUT_COL_START + cursor_pos < FB_COLS) {
    fbputchar('_', INPUT_ROW, INPUT_COL_START + cursor_pos);
  }
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

/* Send the current input line to the server (followed by newline),
 * then clear the input line.
 */
static void input_send_and_clear(void)
{
  /* If empty, still send newline (optional).
   * For demo it might be fine to send empty lines too.
   */
  if (sockfd >= 0) {
    /* Send exactly what was typed */
    if (input_len > 0) {
      write(sockfd, input_line, input_len);
    }
    /* Send a newline to terminate the message */
    write(sockfd, "\n", 1);
  }

  /* Clear the input buffer */
  input_len = 0;
  cursor_pos = 0;
  input_line[0] = '\0';
}

/* new end  */

int main()
{
  int err, col;

  struct sockaddr_in serv_addr;

  struct usb_keyboard_packet packet;
  int transferred;
  char keystate[12];

/* new begin */
  struct usb_keyboard_packet prev_packet;
  memset(&prev_packet, 0, sizeof(prev_packet));

  /* Initialize the input buffer */
  input_len = 0;
  cursor_pos = 0;
  input_line[0] = '\0';
  /* new end */

  if ((err = fbopen()) != 0) {
    fprintf(stderr, "Error: Could not open framebuffer: %d\n", err);
    exit(1);
  }

  /* Draw rows of asterisks across the top and bottom of the screen */
  for (col = 0 ; col < 64 ; col++) {
    fbputchar('*', 0, col);
    fbputchar('*', 23, col);
  }

  fbputs("Hello CSEE 4840 World!", 4, 10);

  /* new begin */
  fbputs("Type here (Enter sends, ESC exits):", 21, 0);
  render_input_line();
  /* new end */

  /* Open the keyboard */
  if ( (keyboard = openkeyboard(&endpoint_address)) == NULL ) {
    fprintf(stderr, "Did not find a keyboard\n");
    exit(1);
  }

  /* Create a TCP communications socket */
  if ( (sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) {
    fprintf(stderr, "Error: Could not create socket\n");
    exit(1);
  }

  /* Get the server address */
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(SERVER_PORT);
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
  for (;;) {
    libusb_interrupt_transfer(keyboard, endpoint_address,
			      (unsigned char *) &packet, sizeof(packet),
			      &transferred, 0);
    if (transferred == sizeof(packet)) {
      //sprintf(keystate, "%02x %02x %02x", packet.modifiers, packet.keycode[0],
	     // packet.keycode[1]);
     // printf("%s\n", keystate);
      fbputs(keystate, 6, 0);
      if (packet.keycode[0] == 0x29) { /* ESC pressed? */
	break;
      }

      //new begin

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

      //new end
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
  char recvBuf[BUFFER_SIZE];
  int n;
  /* Receive data */
  while ( (n = read(sockfd, &recvBuf, BUFFER_SIZE - 1)) > 0 ) {
    recvBuf[n] = '\0';
    //printf("%s", recvBuf);
    fbputs(recvBuf, 8, 0);
  }

  return NULL;
}