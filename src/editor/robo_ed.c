/* MegaZeux
 *
 * Copyright (C) 2004 Gilead Kutnick <exophase@adelphia.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

// Reconstructed robot editor. This is only a shell - the actual
// robot assembly/disassembly code is in rasm.cpp.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include <math.h>

#include "../rasm.h"
#include "../world.h"
#include "../event.h"
#include "../window.h"
#include "../graphics.h"
#include "../intake.h"
#include "../helpsys.h"
#include "../fsafeopen.h"
#include "../configure.h"
#include "../util.h"

#include "char_ed.h"
#include "edit.h"
#include "param.h"
#include "robo_ed.h"
#include "window.h"

#ifdef __WIN32__
#include <windows.h>
#endif

#ifdef CONFIG_X11
#include "SDL.h"
#include "SDL_syswm.h"
#endif

#define combine_colors(a, b)  \
  (a) | (b << 4)              \

// fix cyclic dependency (could be done in several ways)
static int execute_named_macro(robot_state *rstate, char *macro_name);

static int copy_buffer_lines;
static int copy_buffer_total_length;
static int last_find_option = -1;
static char search_string[256];
static char replace_string[256];
static int wrap_option = 1;

static char key_help[(81 * 3) + 1] =
{
  " F1:Help  F2:Color  F3:Char  F4:Param  F5:Char edit  F6-F10:Macros  (see Alt+O) \n"
  " Alt+Home/S:Mark start  Alt+End/E:Mark end  Alt+U:Unmark   Alt+B:Block Action   \n"
  " Alt+Ins:Paste  Alt+X:Export  Alt+I:Import  Alt+V:Verify  Ctrl+I/D/C:Valid Mark \n"
};

static char key_help_hide[82] =
  "     Press Alt + H to view hotkey information.  Press F1 for extended help.     \n";

// Default colors for color coding:
// current line - 11
// immediates - 10
// characters - 14
// colors - color box or 2
// directions - 3
// things - 11
// params - 2
// strings - 14
// equalities - 0
// conditions - 15
// items - 11
// extras - 7
// commands and command fragments - 15

static const char top_line_connect = 194;
static const char bottom_line_connect = 193;
static const char vertical_line = 179;
static const char horizontal_line = 196;
static const char top_char = 219;
static const char bottom_char = 219;
static const char bg_char = 32;
static const char bg_color = 8;
static const char bg_color_solid = combine_colors(0, 8);
static const char top_color = 4;
static const char bottom_color = 1;
static const char line_color = combine_colors(15, 8);
static const char top_text_color = combine_colors(15, 4);
static const char bottom_text_color = combine_colors(15, 1);
static const char top_highlight_color = combine_colors(14, 4);
static const char current_line_color = combine_colors(11, 8);
static const char mark_color = combine_colors(0, 7);

static const int max_size = 65535;

static char **copy_buffer = NULL;
static int case_option = 0;

char macros[5][64];

static void str_lower_case(char *str, char *dest)
{
  int i = 0;
  char c = str[0];

  while(c)
  {
    dest[i] = tolower(c);
    i++;
    c = str[i];
  }

  dest[i] = 0;
}

static void insert_string(char *dest, char *string, int *position)
{
  int n_pos = *position;
  int remainder = strlen(dest + n_pos) + 1;
  int insert_length = strlen(string);

  memmove(dest + n_pos + insert_length, dest + n_pos, remainder);
  memcpy(dest + n_pos, string, insert_length);
  *position = n_pos + insert_length;
}

static void add_blank_line(robot_state *rstate, int relation)
{
  if(rstate->size + 3 < rstate->max_size)
  {
    robot_line *new_rline = malloc(sizeof(robot_line));
    robot_line *current_rline = rstate->current_rline;
    int current_line = rstate->current_line;

    new_rline->line_text_length = 0;
    new_rline->line_text = malloc(1);
    new_rline->line_bytecode = malloc(3);
    new_rline->line_text[0] = 0;
    new_rline->line_bytecode[0] = 1;
    new_rline->line_bytecode[1] = 47;
    new_rline->line_bytecode[2] = 1;
    new_rline->line_bytecode_length = 3;
    new_rline->validity_status = valid;

    rstate->total_lines++;
    rstate->size += 3;

    if(relation > 0)
    {
      new_rline->next = current_rline->next;
      new_rline->previous = current_rline;

      if(current_rline->next)
        current_rline->next->previous = new_rline;

      current_rline->next = new_rline;

      rstate->current_rline = new_rline;

      if(rstate->mark_mode)
      {
        if(rstate->mark_start > current_line)
          rstate->mark_start++;

        if(rstate->mark_end > current_line)
          rstate->mark_end++;
      }
    }
    else
    {
      new_rline->next = current_rline;
      new_rline->previous = current_rline->previous;

      current_rline->previous->next = new_rline;
      current_rline->previous = new_rline;

      if(rstate->mark_mode)
      {
        if(rstate->mark_start >= current_line)
          rstate->mark_start++;

        if(rstate->mark_end >= current_line)
          rstate->mark_end++;
      }
    }

    rstate->current_line = current_line + 1;
  }
}

static void delete_current_line(robot_state *rstate, int move)
{
  if(rstate->total_lines != 1)
  {
    robot_line *current_rline = rstate->current_rline;
    int last_size = current_rline->line_bytecode_length;
    robot_line *next = current_rline->next;
    robot_line *previous = current_rline->previous;

    previous->next = next;

    if(next)
      next->previous = previous;

    free(current_rline->line_text);
    free(current_rline->line_bytecode);
    free(current_rline);

    rstate->size -= last_size;

    if(rstate->mark_mode)
    {
      if(rstate->mark_start_rline == current_rline)
        rstate->mark_start_rline = current_rline->next;

      if(rstate->mark_end_rline == current_rline)
        rstate->mark_end_rline = current_rline->previous;

      if(rstate->mark_start > rstate->current_line)
        rstate->mark_start--;

      if(rstate->mark_end >= rstate->current_line)
        rstate->mark_end--;

      if(rstate->mark_start > rstate->mark_end)
        rstate->mark_mode = 0;
    }

    if(move > 0)
    {
      if(next)
      {
        rstate->current_rline = next;
      }
      else
      {
        rstate->current_rline = previous;
        rstate->current_line--;
      }
    }
    else
    {
      if(rstate->current_line != 1)
      {
        rstate->current_rline = previous;
        rstate->current_line--;
      }
      else
      {
        rstate->current_rline = next;
      }
    }

    strcpy(rstate->command_buffer, rstate->current_rline->line_text);

    rstate->total_lines--;
  }
}

static void macro_default_values(robot_state *rstate, ext_macro *macro_src)
{
  macro_type *current_type;
  int i, i2;

  for(i = 0, current_type = macro_src->types;
   i < macro_src->num_types; i++, current_type++)
  {
    for(i2 = 0; i2 < current_type->num_variables; i2++)
    {
      if(current_type->type == string)
      {
        if(current_type->variables[i2].def.str_storage)
        {
          strcpy(current_type->variables[i2].storage.str_storage,
           current_type->variables[i2].def.str_storage);
        }
        else
        {
          current_type->variables[i2].storage.str_storage[0] = 0;
        }
      }
      else
      {
        memcpy(&(current_type->variables[i2].storage),
         &(current_type->variables[i2].def), sizeof(variable_storage));
      }
    }
  }
}

static int update_current_line(robot_state *rstate)
{
  char bytecode_buffer[COMMAND_BUFFER_LEN];
  char error_buffer[COMMAND_BUFFER_LEN];
  char new_command_buffer[COMMAND_BUFFER_LEN];
  char arg_types[32];
  robot_line *current_rline = rstate->current_rline;
  char *command_buffer = rstate->command_buffer;
  int arg_count;
  int bytecode_length =
   assemble_line(command_buffer, bytecode_buffer, error_buffer,
   arg_types, &arg_count);
  int last_bytecode_length = current_rline->line_bytecode_length;
  int current_size = rstate->size;
  validity_types use_type = rstate->default_invalid;

  // Trigger macro expansion; if the macro doesn't exist, do nothing
  if(command_buffer[0] == '#')
    if(!execute_named_macro(rstate, command_buffer + 1))
      return -1;

  if((bytecode_length != -1) &&
   (current_size + bytecode_length - last_bytecode_length) <=
   rstate->max_size)
  {
    char *next;
    int line_text_length;

    disassemble_line(bytecode_buffer, &next, new_command_buffer,
     error_buffer, &line_text_length, rstate->include_ignores,
     arg_types, &arg_count, rstate->disassemble_base);

    if(line_text_length < 241)
    {
      current_rline->line_text_length = line_text_length;
      current_rline->line_text =
       realloc(current_rline->line_text, line_text_length + 1);

      current_rline->line_bytecode_length = bytecode_length;
      current_rline->line_bytecode =
       realloc(current_rline->line_bytecode, bytecode_length);
      current_rline->num_args = arg_count;
      current_rline->validity_status = valid;

      memcpy(current_rline->arg_types, arg_types, 20);
      strcpy(current_rline->line_text, new_command_buffer);
      memcpy(current_rline->line_bytecode, bytecode_buffer, bytecode_length);

      rstate->size = current_size + bytecode_length - last_bytecode_length;
    }
    else
    {
      current_rline->line_text_length = 240;
      current_rline->line_text =
       realloc(current_rline->line_text, 240 + 1);
      memcpy(current_rline->line_text, new_command_buffer, 240);
      current_rline->line_text[240] = 0;

      if((current_rline->validity_status != valid) &&
       (current_rline->validity_status != invalid_comment))
      {
        use_type = current_rline->validity_status;
      }

      current_rline->line_bytecode_length = 0;
      current_rline->validity_status = use_type;
      rstate->size = current_size - last_bytecode_length;
    }
  }
  else
  {
    int line_text_length;

    command_buffer[240] = 0;
    line_text_length = strlen(command_buffer);

    if(current_rline->validity_status != valid)
      use_type = current_rline->validity_status;

    current_rline->line_text =
     realloc(current_rline->line_text, line_text_length + 1);

    if(use_type == invalid_comment &&
     (current_size + (line_text_length + 5) - last_bytecode_length) >
     rstate->max_size)
    {
      use_type = invalid_uncertain;
    }

    if((use_type == invalid_comment) && (line_text_length > 236))
    {
      line_text_length = 236;
      command_buffer[236] = 0;
    }

    current_rline->line_text_length = line_text_length;
    strcpy(current_rline->line_text, command_buffer);

    if(use_type != invalid_comment)
    {
      current_rline->line_bytecode_length = 0;
      current_rline->validity_status = use_type;
      rstate->size = current_size - last_bytecode_length;
    }
    else
    {
      current_rline->validity_status = invalid_comment;
      current_rline->line_bytecode_length = line_text_length + 5;
      rstate->size =
       current_size + (line_text_length + 5) - last_bytecode_length;
    }
  }

  return 0;
}

static void add_line(robot_state *rstate)
{
  robot_line *new_rline = malloc(sizeof(robot_line));
  robot_line *current_rline = rstate->current_rline;
  new_rline->line_text = NULL;
  new_rline->line_bytecode = NULL;
  new_rline->line_bytecode_length = 0;
  new_rline->validity_status = valid;

  rstate->current_rline = new_rline;
  new_rline->next = current_rline;
  new_rline->previous = current_rline->previous;

  current_rline->previous->next = new_rline;
  current_rline->previous = new_rline;

  if(update_current_line(rstate) != -1)
  {

    rstate->current_rline = current_rline;

    if(rstate->mark_mode)
    {
      if(rstate->mark_start >= rstate->current_line)
        rstate->mark_start++;

      if(rstate->mark_end >= rstate->current_line)
        rstate->mark_end++;
    }

    rstate->total_lines++;
    rstate->current_line++;
  }
  else
  {
    current_rline->previous = new_rline->previous;
    current_rline->previous->next = new_rline->next;
    rstate->current_rline = current_rline;
    free(new_rline);
  }
}

static void output_macro(robot_state *rstate, ext_macro *macro_src)
{
  int num_lines = macro_src->num_lines;
  char line_buffer[COMMAND_BUFFER_LEN];
  char number_buffer[16];
  char *line_pos, *line_pos_old;
  char *old_buffer_space = rstate->command_buffer;
  macro_variable_reference *current_reference;
  int i, i2;
  int len;

  if(rstate->macro_recurse_level == MAX_MACRO_RECURSION ||
   rstate->macro_repeat_level == MAX_MACRO_REPEAT)
  {
    rstate->command_buffer[0] = 0;
    update_current_line(rstate);
    return;
  }

  rstate->macro_recurse_level++;
  rstate->macro_repeat_level++;

  // OK, output the lines

  rstate->command_buffer = line_buffer;

  for(i = 0; i < num_lines; i++)
  {
    line_pos = line_buffer;
    line_pos_old = line_pos;
    len = strlen(macro_src->lines[i][0]);
    strcpy(line_pos, macro_src->lines[i][0]);
    line_pos += len;

    for(i2 = 0; i2 < macro_src->line_element_count[i]; i2++)
    {
      current_reference = &(macro_src->variable_references[i][i2]);
      if(current_reference->type)
      {
        switch(current_reference->type->type)
        {
          case number:
          {
            if(current_reference->ref_mode == hexidecimal)
            {
              sprintf(number_buffer, "%02x",
               current_reference->storage->int_storage);
            }
            else
            {
              sprintf(number_buffer, "%d",
               current_reference->storage->int_storage);
            }

            len = strlen(number_buffer);
            memcpy(line_pos, number_buffer, len);
            line_pos += len;
            break;
          }

          case string:
          {
            len = strlen(current_reference->storage->str_storage);
            memcpy(line_pos, current_reference->storage->str_storage,
             len);
            line_pos += len;
            break;
          }

          case character:
          {
            sprintf(line_pos, "'%c'",
             current_reference->storage->int_storage);
            line_pos += 3;
            break;
          }

          case color:
          {
            print_color(current_reference->storage->int_storage,
             line_pos);
            line_pos += 3;
            break;
          }

          default:
          {
            break;
          }
        }
      }
      else
      {
        strcpy(line_pos, "(undef)");
        line_pos += strlen("(undef)");
      }

      len = strlen(macro_src->lines[i][i2 + 1]);
      memcpy(line_pos, macro_src->lines[i][i2 + 1], len);
      line_pos += len;
    }

    *line_pos = 0;
    add_line(rstate);
  }

  rstate->command_buffer = old_buffer_space;
  rstate->macro_recurse_level--;
}

static int execute_named_macro(robot_state *rstate, char *macro_name)
{
  char *line_pos, *line_pos_old, *lone_name;
  macro_type *param_type = NULL;
  ext_macro *macro_src;
  char last_char;
  int next;

  line_pos = skip_to_next(macro_name, ',', '(', 0);

  // extract just the name of the macro, if valid
  lone_name = malloc(line_pos - macro_name + 1);
  memcpy(lone_name, macro_name, line_pos - macro_name);
  lone_name[line_pos - macro_name] = 0;

  // see if such a macro exists
  macro_src = find_macro(&(rstate->mzx_world->conf), macro_name, &next);
  free(lone_name);

  // it doesn't, carefully abort
  if(!macro_src)
    return 1;

  last_char = *line_pos;

  if(macro_src->num_types && last_char)
  {
    variable_storage *param_storage;
    int param_current_type = 0;
    int param_current_var = 0;
    char *value = NULL;
    int i;

    // Fill in parameters
    macro_default_values(rstate, macro_src);

    // Get name, may stop at equals sign first
    while(1)
    {
      line_pos = skip_whitespace(line_pos + 1);
      line_pos_old = line_pos;
      line_pos = skip_to_next(line_pos, '=', ',', ')');

      last_char = *line_pos;
      *line_pos = 0;

      param_storage = NULL;

      if(last_char == '=')
      {
        // It's named, find which index this corresponds to.
        *line_pos = 0;
        param_type = macro_src->types;

        for(i = 0; i < macro_src->num_types; i++, param_type++)
        {
          param_storage = find_macro_variable(line_pos_old,
           param_type);
          if(param_storage)
            break;
        }

        line_pos++;
        value = line_pos;
        line_pos = skip_to_next(line_pos, '=', ',', ')');
        last_char = *line_pos;
        *line_pos = 0;
      }
      else

      if(*(line_pos - 1) != '(')
      {
        param_type = macro_src->types + param_current_type;

        if(param_current_var == param_type->num_variables)
        {
          param_current_type++;
          param_type++;
          param_current_var = 0;
        }

        if(param_current_type < macro_src->num_types)
        {
          param_storage =
           &((param_type->variables[param_current_var]).storage);
        }

        value = line_pos_old;
        param_current_var++;
      }

      if(param_storage)
      {
        switch(param_type->type)
        {
          case number:
          {
            param_storage->int_storage = strtol(value, NULL, 10);

            if(param_storage->int_storage <
             param_type->type_attributes[0])
            {
              param_storage->int_storage =
               param_type->type_attributes[0];
            }

            if(param_storage->int_storage >
             param_type->type_attributes[1])
            {
              param_storage->int_storage =
               param_type->type_attributes[1];
            }

            break;
          }

          case string:
          {
            value[param_type->type_attributes[0]] = 0;
            strcpy(param_storage->str_storage, value);
            break;
          }

          case character:
          {
            if(*value == '\'')
            {
              param_storage->int_storage =
               *(value + 1);
            }
            else
            {
              param_storage->int_storage =
               strtol(value, NULL, 10);
            }
            break;
          }

          case color:
          {
            param_storage->int_storage =
             get_color(value);
            break;
          }
        }
      }

      if((last_char == ')') || (!last_char))
        break;
    }
  }

  output_macro(rstate, macro_src);
  return 0;
}

static int block_menu(World *mzx_world)
{
  int dialog_result, block_op = 0;
  dialog di;
  const char *radio_strings[] =
  {
    "Copy block", "Cut block",
    "Clear block", "Export block"
  };
  element *elements[3] =
  {
    construct_radio_button(2, 2, radio_strings,
     4, 21, &block_op),
    construct_button(5, 7, "OK", 0),
    construct_button(15, 7, "Cancel", -1)
  };

  construct_dialog(&di, "Choose Block Command", 26, 6, 28, 10,
   elements, 3, 0);

  dialog_result = run_dialog(mzx_world, &di);
  destruct_dialog(&di);

  if(dialog_result == -1)
    return -1;
  else
    return block_op;
}

#ifdef CONFIG_X11
static int copy_buffer_to_X11_selection(const SDL_Event *event)
{
  if(event->type != SDL_SYSWMEVENT)
    return 1;

  if((event->syswm.msg->event.xevent.type == SelectionRequest) && copy_buffer)
  {
    SDL_SysWMinfo info;
    Display *display;
    Window window;
    char *dest_data, *dest_ptr;
    XSelectionRequestEvent *request;
    XEvent response;
    int i, line_length;

    SDL_VERSION(&info.version);
    SDL_GetWMInfo(&info);

    display = info.info.x11.display;
    window = info.info.x11.window;
    dest_data = malloc(copy_buffer_total_length + 1);
    dest_ptr = dest_data;

    for(i = 0; i < copy_buffer_lines - 1; i++)
    {
      line_length = strlen(copy_buffer[i]);
      memcpy(dest_ptr, copy_buffer[i], line_length);
      dest_ptr += line_length;

      dest_ptr[0] = '\n';
      dest_ptr++;
    }

    line_length = strlen(copy_buffer[i]);
    memcpy(dest_ptr, copy_buffer[i], line_length);
    dest_ptr[line_length] = 0;

    request = &(event->syswm.msg->event.xevent.xselectionrequest);
    response.xselection.type = SelectionNotify;
    response.xselection.display = request->display;
    response.xselection.selection = request->selection;
    response.xselection.target = XA_STRING;
    response.xselection.property = request->property;
    response.xselection.requestor = request->requestor;
    response.xselection.time = request->time;

    XChangeProperty(display, request->requestor, request->property,
     XA_STRING, 8, PropModeReplace, (const unsigned char *)dest_data,
     copy_buffer_total_length);

    free(dest_data);

    XSendEvent(display, request->requestor, True, 0, &response);
    XFlush(display);
  }

  return 0;
}
#endif /* CONFIG_X11 */

static void copy_block_to_buffer(robot_state *rstate)
{
  robot_line *current_rline = rstate->mark_start_rline;
  int num_lines = (rstate->mark_end - rstate->mark_start) + 1;
  int line_length;
  int i;

#ifdef CONFIG_X11
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
#endif

  copy_buffer_total_length = 0;

  // First, if there's something already there, clear all the lines and
  // the buffer.
  if(copy_buffer)
  {
    for(i = 0; i < copy_buffer_lines; i++)
    {
      free(copy_buffer[i]);
    }

    free(copy_buffer);
  }

  copy_buffer = calloc(num_lines, sizeof(char *));
  copy_buffer_lines = num_lines;

  for(i = 0; i < num_lines; i++)
  {
    line_length = current_rline->line_text_length + 1;
    copy_buffer[i] = malloc(COMMAND_BUFFER_LEN);
    memcpy(copy_buffer[i], current_rline->line_text, line_length);
    current_rline = current_rline->next;

    copy_buffer_total_length += line_length;
  }

#ifdef __WIN32__
  if(OpenClipboard(NULL))
  {
    HANDLE global_memory;
    char *dest_data;
    char *dest_ptr;

    // Room for \r's
    copy_buffer_total_length += num_lines - 1;

    global_memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE,
     copy_buffer_total_length);
    if(global_memory)
    {
      dest_data = (char *)GlobalLock(global_memory);
      dest_ptr = dest_data;

      current_rline = rstate->mark_start_rline;

      for(i = 0; i < num_lines - 1; i++)
      {
        line_length = current_rline->line_text_length;
        memcpy(dest_ptr, current_rline->line_text, line_length);
        dest_ptr += line_length;

        dest_ptr[0] = '\r';
        dest_ptr[1] = '\n';
        dest_ptr += 2;

        current_rline = current_rline->next;
      }

      line_length = current_rline->line_text_length;
      memcpy(dest_ptr, current_rline->line_text, line_length);
      dest_ptr += line_length;
      dest_ptr[0] = 0;

      GlobalUnlock(global_memory);
      EmptyClipboard();
      SetClipboardData(CF_TEXT, global_memory);
    }
    CloseClipboard();
  }
#elif defined(CONFIG_X11)
  if(SDL_GetWMInfo(&info) && (info.subsystem == SDL_SYSWM_X11))
  {
    XSetSelectionOwner(info.info.x11.display, XA_PRIMARY,
     info.info.x11.window, CurrentTime);

    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);

    SDL_SetEventFilter(copy_buffer_to_X11_selection);
  }
#endif // CONFIG_X11
}

static void clear_block(robot_state *rstate)
{
  robot_line *current_rline = rstate->mark_start_rline;
  robot_line *line_after = rstate->mark_end_rline->next;
  robot_line *line_before = current_rline->previous;
  robot_line *next_rline;
  int num_lines = rstate->mark_end - rstate->mark_start + 1;
  int i;

  for(i = 0; i < num_lines; i++)
  {
    next_rline = current_rline->next;

    rstate->size -= current_rline->line_bytecode_length;
    free(current_rline->line_text);
    free(current_rline->line_bytecode);
    free(current_rline);

    current_rline = next_rline;
  }

  line_before->next = line_after;
  if(line_after)
    line_after->previous = line_before;

  rstate->total_lines -= num_lines;

  if(!rstate->total_lines)
  {
    rstate->current_line = 0;
    rstate->current_rline = rstate->base;
    add_blank_line(rstate, 1);
  }
  else
  {
    if(rstate->current_line >= rstate->mark_start)
    {
      if(rstate->current_line <= rstate->mark_end)
      {
        // Current line got swallowed.
        if(line_after)
        {
          rstate->current_line = rstate->mark_start;
          rstate->current_rline = line_after;
        }
        else
        {
          rstate->current_line = rstate->mark_start - 1;
          rstate->current_rline = line_before;
        }
      }
      else
      {
        rstate->current_line -= num_lines;
      }
    }
  }

  strcpy(rstate->command_buffer, rstate->current_rline->line_text);
}

static void export_block(robot_state *rstate, int region_default)
{
  World *mzx_world = rstate->mzx_world;
  robot_line *current_rline;
  robot_line *end_rline;
  int export_region = region_default;
  int export_type = 0;
  char export_name[64];
  int num_elements;
  const char *export_ext[] = { ".TXT", ".BC", NULL };
  const char *radio_strings_1[] =
  {
    "Text", "Bytecode"
  };
  const char *radio_strings_2[] =
  {
    "Entire robot", "Current block"
  };
  element *elements[4];

  if(region_default)
  {
    elements[0] = construct_label(45, 19,
     "Export\nregion as: ");
    elements[1] = construct_radio_button(56,
     19, radio_strings_1, 2, 8, &export_type);
    elements[2] = construct_label(4, 19,
     "Export the\nfollowing region: ");
    elements[3] = construct_radio_button(22, 19,
     radio_strings_2, 2, 13, &export_region);
    num_elements = 4;
  }
  else
  {
    elements[0] = construct_label(25, 19,
     "Export\nregion as: ");
    elements[1] = construct_radio_button(36,
     19, radio_strings_1, 2, 8, &export_type);
    num_elements = 2;
  }

  export_name[0] = 0;

  if(!file_manager(mzx_world, export_ext, export_name,
   "Export robot", 1, 1, elements, num_elements, 3, 0))
  {
    FILE *export_file;

    if(!export_region)
    {
      current_rline = rstate->base->next;
      end_rline = NULL;
    }
    else
    {
      current_rline = rstate->mark_start_rline;
      end_rline = rstate->mark_end_rline->next;
    }

    if(!export_type)
    {
      add_ext(export_name, ".txt");
      export_file = fopen(export_name, "w");

      while(current_rline != end_rline)
      {
        fputs(current_rline->line_text, export_file);
        fputc('\n', export_file);
        current_rline = current_rline->next;
      }
    }
    else
    {
      add_ext(export_name, ".bc");
      export_file = fopen(export_name, "wb");

      fputc(0xFF, export_file);

      while(current_rline != end_rline)
      {
        fwrite(current_rline->line_bytecode,
         current_rline->line_bytecode_length, 1, export_file);
        current_rline = current_rline->next;
      }

      fputc(0, export_file);
    }

    fclose(export_file);
  }
}

static void import_block(World *mzx_world, robot_state *rstate)
{
  const char *txt_ext[] = { ".TXT", ".BC", NULL };
  char import_name[128];
  char line_buffer[256];
  FILE *import_file;
  ssize_t ext_pos;

  if(choose_file(mzx_world, txt_ext, import_name, "Import Robot", 1))
    return;

  import_file = fopen(import_name, "r");
  ext_pos = strlen(import_name) - 3;

  rstate->command_buffer = line_buffer;

  if(ext_pos < 1 || strcasecmp(import_name + ext_pos, ".BC"))
  {
    // fsafegets ensures that no line terminators are present
    while(fsafegets(line_buffer, 255, import_file) != NULL)
      add_line(rstate);
  }
  else
  {
    long file_size = ftell_and_rewind(import_file);

    // 0xff + length + cmd + length + 0x00
    if(file_size >= 5)
    {
       char *buffer = malloc(file_size - 1);
       size_t ret;

       // skip 0xff
       fgetc(import_file);

       // put the rest in a buffer for disassemble_line()
       ret = fread(buffer, file_size - 1, 1, import_file);

       // copied to buffer, must now disassemble
       if(ret == 1)
       {
         char *current_robot_pos = buffer, *next;
         char error_buffer[256];
         int line_text_length;

         while(1)
         {
           int new_line = disassemble_line(current_robot_pos, &next,
            line_buffer, error_buffer, &line_text_length,
            mzx_world->conf.disassemble_extras, NULL, NULL,
            mzx_world->conf.disassemble_base);

           if(new_line)
             add_line(rstate);
           else
             break;

           current_robot_pos = next;
         }
       }

       free(buffer);
    }
  }

  rstate->command_buffer = rstate->command_buffer_space;

  fclose(import_file);
}

static void edit_settings(World *mzx_world)
{
  // 38 x 12
  int dialog_result;
  dialog di;
  element *elements[8] =
  {
    construct_label(5, 2, "Macros:"),
    construct_input_box(5, 4, "F6-  ", 43, 0, macros[0]),
    construct_input_box(5, 5, "F7-  ", 43, 0, macros[1]),
    construct_input_box(5, 6, "F8-  ", 43, 0, macros[2]),
    construct_input_box(5, 7, "F9-  ", 43, 0, macros[3]),
    construct_input_box(5, 8, "F10- ", 43, 0, macros[4]),
    construct_button(15, 10, "OK", 0),
    construct_button(37, 10, "Cancel", -1)
  };
  char new_macros[5][64];

  memcpy(new_macros, macros, 64 * 5);

  construct_dialog(&di, "Edit Settings", 10, 6, 60, 12,
   elements, 8, 1);

  dialog_result = run_dialog(mzx_world, &di);
  destruct_dialog(&di);

  if(dialog_result)
    memcpy(macros, new_macros, 64 * 5);
}

static void block_action(robot_state *rstate)
{
  World *mzx_world = rstate->mzx_world;
  int block_command = block_menu(mzx_world);

  if(!rstate->mark_mode)
  {
    rstate->mark_start_rline = rstate->current_rline;
    rstate->mark_end_rline = rstate->current_rline;
    rstate->mark_start = rstate->current_line;
    rstate->mark_end = rstate->current_line;
  }

  switch(block_command)
  {
    case 0:
    {
      // Copy block into buffer
      copy_block_to_buffer(rstate);
      break;
    }

    case 1:
    {
      // Copy block into buffer, then clear block
      copy_block_to_buffer(rstate);
      clear_block(rstate);
      rstate->mark_mode = 0;
      break;
    }

    case 2:
    {
      // Clear block
      clear_block(rstate);
      rstate->mark_mode = 0;
      break;
    }

    case 3:
    {
      // Export block - text only
      export_block(rstate, 1);
      break;
    }
  }
}

static void move_line_up(robot_state *rstate, int count)
{
  int i;

  for(i = 0; (i < count); i++)
  {
    if(rstate->current_rline->previous->line_bytecode_length == -1)
      break;

    rstate->current_rline = rstate->current_rline->previous;
  }

  rstate->current_line -= i;
}

static void move_line_down(robot_state *rstate, int count)
{
  int i;

  for(i = 0; (i < count); i++)
  {
    if(rstate->current_rline->next == NULL)
    {
      // Add a new line if the last line isn't blank.
      if(rstate->current_rline->line_text[0])
        add_blank_line(rstate, 1);

      break;
    }

    rstate->current_rline = rstate->current_rline->next;
  }

  rstate->current_line += i;
}

static void move_and_update(robot_state *rstate, int count)
{
  update_current_line(rstate);
  if(count < 0)
  {
    move_line_up(rstate, -count);
  }
  else
  {
    move_line_down(rstate, count);
  }

  strcpy(rstate->command_buffer, rstate->current_rline->line_text);
}

static void goto_line(robot_state *rstate, int line)
{
  if(line > rstate->total_lines)
    line = rstate->total_lines;

  if(line < 1)
    line = 1;

  move_and_update(rstate, line - rstate->current_line);
}

static void goto_position(World *mzx_world, robot_state *rstate)
{
  int dialog_result;
  int line_number = rstate->current_line;
  int column_number = rstate->current_x;
  dialog di;

  element *elements[4] =
  {
    construct_number_box(2, 2, "Line:   ", 0, rstate->total_lines,
     0, &line_number),
    construct_number_box(2, 3, "Column: ", 0, 240, 0, &column_number),
    construct_button(3, 5, "OK", 0),
    construct_button(14, 5, "Cancel", -1)
  };

  construct_dialog(&di, "Goto position", 28, 8, 25, 7,
   elements, 4, 0);

  dialog_result = run_dialog(mzx_world, &di);
  destruct_dialog(&di);

  if(dialog_result != -1)
  {
    rstate->current_x = column_number;
    goto_line(rstate, line_number);
  }
}

static void replace_current_line(robot_state *rstate, int r_pos, char *str,
 char *replace)
{
  robot_line *current_rline = rstate->current_rline;
  char new_buffer[COMMAND_BUFFER_LEN];
  int replace_size = strlen(replace);
  int str_size = strlen(str);

  strncpy(new_buffer, current_rline->line_text, COMMAND_BUFFER_LEN - 1);
  new_buffer[COMMAND_BUFFER_LEN - 1] = '\0';

  memmove(new_buffer + r_pos + replace_size,
   new_buffer + r_pos + str_size, strlen(new_buffer) - r_pos);
  memcpy(new_buffer + r_pos, replace, replace_size);
  new_buffer[240] = '\0';

  rstate->command_buffer = new_buffer;
  update_current_line(rstate);

  strncpy(rstate->command_buffer_space, new_buffer, COMMAND_BUFFER_LEN - 1);
  rstate->command_buffer_space[COMMAND_BUFFER_LEN - 1] = '\0';
  rstate->command_buffer = rstate->command_buffer_space;
}

static int robo_ed_find_string(robot_state *rstate, char *str, int wrap,
 int *position, int case_sensitive)
{
  robot_line *current_rline = rstate->current_rline;
  int current_line = rstate->current_line;
  char *pos = NULL;
  char line_buffer[COMMAND_BUFFER_LEN];
  char *use_buffer = line_buffer;

  update_current_line(rstate);
  strcpy(rstate->command_buffer, current_rline->line_text);

  if(!str[0])
    return -1;

  if(!case_sensitive)
    str_lower_case(str, str);

  // Check the first line first

  if(!case_sensitive)
    str_lower_case(current_rline->line_text, line_buffer);
  else
    use_buffer = current_rline->line_text;

  pos = strstr(use_buffer + rstate->current_x + 1, str);

  if(pos == NULL)
  {
    current_rline = current_rline->next;
    current_line++;

    // Now check the next lines
    while(current_rline != NULL)
    {
      if(!case_sensitive)
        str_lower_case(current_rline->line_text, line_buffer);
      else
        use_buffer = current_rline->line_text;

      pos = strstr(use_buffer, str);

      if(pos)
        break;

      current_rline = current_rline->next;
      current_line++;
    }
  }

  // Wrap around?
  if(wrap && (pos == NULL))
  {
    current_rline = rstate->base->next;
    current_line = 1;
    while(current_rline != rstate->current_rline->next)
    {
      if(!case_sensitive)
        str_lower_case(current_rline->line_text, line_buffer);
      else
        use_buffer = current_rline->line_text;

      pos = strstr(use_buffer, str);

      if(pos)
        break;

      current_rline = current_rline->next;
      current_line++;
    }
  }

  if(pos)
  {
    *position = pos - use_buffer;
    return current_line;
  }

  return -1;
}

static void find_replace_action(robot_state *rstate)
{
  dialog di;
  const char *check_strings_1[] = { "Wrap around end" };
  const char *check_strings_2[] = { "Match case" };
  int check_result_1[] = { wrap_option };
  int check_result_2[] = { case_option };
  element *elements[8] =
  {
    construct_input_box(2, 2, "Find:    ",
     46, 0, search_string),
    construct_input_box(2, 3, "Replace: ",
     46, 0, replace_string),
    construct_check_box(3, 5, check_strings_1,
     1, 15, check_result_1),
    construct_check_box(30, 5, check_strings_2,
     1, 10, check_result_2),
    construct_button(5, 7, "Search", 0),
    construct_button(15, 7, "Replace", 1),
    construct_button(27, 7, "Replace All", 2),
    construct_button(44, 7, "Cancel", -1)
  };

  construct_dialog(&di, "Search and Replace", 10, 7, 70, 10,
   elements, 8, 0);

  last_find_option = run_dialog(rstate->mzx_world, &di);
  destruct_dialog(&di);

  wrap_option = check_result_1[0];
  case_option = check_result_2[0];

  switch(last_find_option)
  {
    case 0:
    {
      // Find
      int l_pos;
      int l_num = robo_ed_find_string(rstate, search_string, wrap_option,
       &l_pos, case_option);

      if(l_num != -1)
      {
        goto_line(rstate, l_num);
        rstate->current_x = l_pos;
      }

      break;
    }

    case 1:
    {
      // Find & Replace
      int l_pos;
      int l_num = robo_ed_find_string(rstate, search_string, wrap_option,
       &l_pos, case_option);

      if(l_num != -1)
      {
        goto_line(rstate, l_num);
        rstate->current_x = l_pos;
        replace_current_line(rstate, l_pos, search_string, replace_string);
      }

      break;
    }

    case 2:
    {
      int l_pos;
      int l_num;
      int r_len = strlen(replace_string);
      int s_len = strlen(search_string);
      int start_line = rstate->current_line;
      int start_pos = rstate->current_x;
      int last_line = start_line;
      int last_pos = start_pos;
      int dif = r_len - s_len;
      int wrapped = 0;

      do
      {
        l_num = robo_ed_find_string(rstate, search_string, wrap_option,
         &l_pos, case_option);

        // Is it on the starting line and below the starting cursor?
        // If so modify the starting cursor because the line was
        // changed.
        if((l_num == start_line) && (l_num <= start_pos))
        {
          start_pos += dif;
        }

        // A decrease had better indicate a wrap, and you only get one
        if(((l_num == last_line) && (l_pos < last_pos)) || (l_num < last_line))
        {
          if((((l_num == start_line) && (l_pos <= start_pos)) ||
           (l_num < start_line)) && (!wrapped))
          {
            wrapped = 1;
          }
          else
          {
            break;
          }
        }

        // If it has already wrapped don't let it past the start
        if((((l_num == start_line) && (l_pos > start_pos)) ||
         (l_num > start_line)) && wrapped)
        {
          break;
        }

        if(l_num != -1 && l_pos <= 240)
        {
          goto_line(rstate, l_num);
          rstate->current_x = l_pos;
          replace_current_line(rstate, l_pos, search_string, replace_string);

          l_pos += r_len;
          rstate->current_x = l_pos;

          last_line = l_num;
          last_pos = l_pos;
        }
        else
        {
          break;
        }
      } while(1);

      break;
    }
  }
}

static void execute_macro(robot_state *rstate, ext_macro *macro_src)
{
  World *mzx_world = rstate->mzx_world;
  macro_type *current_type;
  int i, i2, i3;

  // First, the dialogue box must be generated. This will have a text
  // label for the variable name and an input widget for the variable's
  // value itself. An attempt will be made to make this look as nice
  // as possible, but these will be comprimised (in this order) if
  // space is critical.
  // 1) All variables within a type group will be be aligned at a
  //    a fixed with (of whatever the largest width of the name +
  //    input widgit is) so that if they take multiple lines the
  //    boxes will line up.
  // 2) A blank line will be placed between each type group
  // 3) The width/height ratio will be made as close to 4:3 as possible,
  //    and the box will be centered.
  // The first two dialogue elements are reserved for okay/cancel

  int total_dialog_elements = macro_src->total_variables + 3;
  int num_types = macro_src->num_types;
  element **elements = malloc(sizeof(element *) * total_dialog_elements);
  int *nominal_column_widths = malloc(sizeof(int) * num_types);
  int *nominal_column_subwidths = malloc(sizeof(int) * num_types);
  int *vars_per_line = malloc(sizeof(int) * num_types);
  int largest_column_width = 0;
  int total_width;
  int largest_total_width;
  int *lines_needed = malloc(sizeof(int) * num_types);
  int nominal_width = 77, old_width = 77;
  int nominal_height, old_height = 25;
  int total_lines_needed;
  int largest;
  int current_len;
  float optimal_delta, old_optimal_delta = 1000000.0;
  int x, y, dialog_index = 2;
  macro_variable *current_variable;
  int draw_on_line;
  int start_x, start_y;
  dialog di;
  int dialog_value;
  int label_size = strlen(macro_src->label) + 1;
  int subwidths[3];

  // No variables? Just print bare lines...
  if(!num_types)
  {
    output_macro(rstate, macro_src);
    goto exit_free;
  }

  current_type = macro_src->types;

  for(i = 0; i < num_types; i++, current_type++)
  {
    largest = strlen(current_type->variables[0].name);
    for(i2 = 1; i2 < current_type->num_variables; i2++)
    {
      current_len = strlen(current_type->variables[i2].name);
      if(current_len > largest)
        largest = current_len;
    }

    nominal_column_subwidths[i] = largest;

    switch(current_type->type)
    {
      case number:
      {
        largest += 15;
        break;
      }

      case string:
      {
        largest += current_type->type_attributes[0] + 3;
        break;
      }

      case character:
      {
        largest += 5;
        break;
      }

      case color:
      {
        largest += 5;
        break;
      }
    }

    nominal_column_widths[i] = largest;

    if(largest > largest_column_width)
      largest_column_width = largest;
  }

  do
  {
    largest_total_width = 0;
    total_lines_needed = 0;

    // Determine how many lines each type needs
    for(i = 0; i < num_types; i++)
    {
      total_width = nominal_width / nominal_column_widths[i];
      if(total_width == 0)
      {
        lines_needed[i] = 1;
      }
      else
      {
        lines_needed[i] = (macro_src->types[i].num_variables +
         (total_width - 1)) / total_width;
      }

      if(total_width > macro_src->types[i].num_variables)
        total_width = macro_src->types[i].num_variables;

      total_width *= nominal_column_widths[i];

      total_lines_needed += lines_needed[i] + 1;
      if(total_width > largest_total_width)
        largest_total_width = total_width;
    }

    nominal_width = largest_total_width;
    // Add a line between each, a top line, two for buttons, and
    // two for borders
    nominal_height = total_lines_needed + 5;

    optimal_delta = fabs((80.0 / 25) -
     (float)(nominal_width + 3) / nominal_height);

    if((old_optimal_delta < optimal_delta) ||
     (nominal_width < largest_column_width) || (nominal_height > 25))
    {
      // Use the old one instead
      nominal_width = old_width;
      nominal_height = old_height;
      break;
    }

    old_width = nominal_width;
    old_height = nominal_height;
    old_optimal_delta = optimal_delta;

    nominal_width -= largest_column_width;
  } while(1);

  for(i = 0; i < num_types; i++)
  {
    vars_per_line[i] = nominal_width / nominal_column_widths[i];
  }

  if(nominal_width < label_size)
    nominal_width = label_size;

  if(nominal_width < 25)
    nominal_width = 25;

  if(nominal_width < 30)
  {
    subwidths[0] = nominal_width - 20;
    subwidths[1] = 10;
  }
  else
  {
    subwidths[0] = nominal_width / 3;
    subwidths[1] = subwidths[0];
  }

  nominal_width += 3;

  // Generate the dialogue box
  start_x = 40 - (nominal_width / 2);
  start_y = 12 - (nominal_height / 2);

  x = 2;
  y = 2;

  current_type = macro_src->types;

  for(i = 0, dialog_index = 3; i < num_types; i++, current_type++)
  {
    current_variable = current_type->variables;
    // Draw this many
    draw_on_line = vars_per_line[i];
    for(i2 = current_type->num_variables; i2 > 0; i2 -= draw_on_line,
     y++)
    {
      if(y >= (nominal_height - 3))
      {
        total_dialog_elements = dialog_index;
        break;
      }

      if(i2 < draw_on_line)
        draw_on_line = i2;

      x += ((nominal_width - 3) / 2) -
       ((nominal_column_widths[i] * draw_on_line) / 2);

      for(i3 = 0; i3 < draw_on_line; i3++, current_variable++,
       dialog_index++)
      {
        current_len = nominal_column_subwidths[i] -
         strlen(current_variable->name);
        x += current_len;

        switch(current_type->type)
        {
          case number:
          {
            elements[dialog_index] =
             construct_number_box(x, y, current_variable->name,
             current_type->type_attributes[0],
             current_type->type_attributes[1], 0,
             &(current_variable->storage.int_storage));
            break;
          }

          case string:
          {
            elements[dialog_index] =
             construct_input_box(x, y, current_variable->name,
             current_type->type_attributes[0], 0,
             current_variable->storage.str_storage);
            break;
          }

          case character:
          {
            elements[dialog_index] =
             construct_char_box(x, y, current_variable->name,
             1, &(current_variable->storage.int_storage));
            break;
          }

          case color:
          {
            elements[dialog_index] =
             construct_color_box(x, y, current_variable->name,
             1, &(current_variable->storage.int_storage));
            break;
          }
        }

        x += nominal_column_widths[i] - current_len;
      }
      // Start over on next line
      x = 2;
    }
    // Start over after skipping a line
    x = 2;

    if(y >= (nominal_height - 3))
      break;

    y++;
  }

  // Place OK/Cancel/Default

  elements[0] =
   construct_button(subwidths[0] / 2, y, "OK", 0);
  elements[1] =
   construct_button(subwidths[0] + (subwidths[1] / 2) - 2, y,
   "Cancel", -1);
  elements[2] =
   construct_button(subwidths[0] + subwidths[1] +
   (subwidths[1] / 2) - 3, y, "Default", -2);

  construct_dialog_ext(&di, macro_src->label, start_x,
   start_y, nominal_width, nominal_height, elements,
   total_dialog_elements, 0, 1, 3, NULL);

  do
  {
    dialog_value = run_dialog(mzx_world, &di);

    switch(dialog_value)
    {
      case 0:
      {
        // OK, output the lines
        output_macro(rstate, macro_src);
        break;
      }

      case -2:
      {
        // Plug in default values, allow to run again
        macro_default_values(rstate, macro_src);
        break;
      }
    }
  } while(dialog_value == -2);

  destruct_dialog(&di);

exit_free:
  free(lines_needed);
  free(vars_per_line);
  free(nominal_column_subwidths);
  free(nominal_column_widths);
  free(elements);
}

static void execute_numbered_macro(robot_state *rstate, int num)
{
  World *mzx_world = rstate->mzx_world;
  ext_macro *macro_src;
  char macro_name[32];
  int next;

  sprintf(macro_name, "%d", num);
  macro_src = find_macro(&(mzx_world->conf), macro_name, &next);

  if(macro_src)
    execute_macro(rstate, macro_src);
  else
    rstate->active_macro = macros[num - 1];
}

static void robo_ed_display_robot_line(robot_state *rstate,
 robot_line *current_rline, int y)
{
  int i;
  int x = 2;
  int current_color, current_arg;
  int color_code = rstate->color_code;
  char *color_codes = rstate->ccodes;
  char temp_char;
  char temp_buffer[COMMAND_BUFFER_LEN];
  char *line_pos = current_rline->line_text;
  int chars_offset = 0;
  int arg_length;
  int use_mask;

  if(current_rline->line_text[0] != 0)
  {
    if(current_rline->validity_status != valid)
    {
      current_color =
       combine_colors(color_codes[S_CMD + current_rline->validity_status + 1],
       bg_color);

      if(strlen(current_rline->line_text) > 76)
      {
        temp_char = current_rline->line_text[76];
        current_rline->line_text[76] = 0;
        write_string_mask(current_rline->line_text, x,
         y, current_color, 0);
        current_rline->line_text[76] = temp_char;
      }
      else
      {
        write_string_mask(current_rline->line_text, x,
         y, current_color, 0);
      }
    }
    else
    {
      use_mask = 0;
      arg_length = 0;

      if(!color_code)
      {
        current_color =
         combine_colors(color_codes[S_STRING + 1], bg_color);
      }

      if(color_code)
        current_color = combine_colors(color_codes[S_CMD + 1], bg_color);

      arg_length = strcspn(line_pos, " ") + 1;
      memcpy(temp_buffer, line_pos, arg_length);
      temp_buffer[arg_length] = 0;

      write_string(temp_buffer, x, y, current_color, 0);

      line_pos += arg_length;
      x += arg_length;

      for(i = 0; i < current_rline->num_args; i++)
      {
        current_arg = current_rline->arg_types[i];

        if(current_arg == S_STRING)
        {
          char current;
          arg_length = 1;

          do
          {
            current = line_pos[arg_length];
            if((current == '\\') && ((line_pos[arg_length + 1] == '"') ||
             (line_pos[arg_length + 1] == '\\')))
            {
              arg_length++;
            }
            arg_length++;

          } while((current != '"') && current);

          arg_length++;

          chars_offset = 0;

          use_mask = rstate->mzx_world->conf.mask_midchars;
        }
        else

        if(current_arg == S_CHARACTER)
        {
          arg_length = strcspn(line_pos + 1, "\'") + 3;
          chars_offset = 0;
        }
        else
        {
          arg_length = strcspn(line_pos, " ") + 1;
          chars_offset = 256;
        }

        if((current_arg == S_COLOR) && (color_codes[current_arg + 1] == 255))
        {
          unsigned int color = get_color(line_pos);
          draw_color_box(color & 0xFF, color >> 8, x, y);
        }
        else
        {
          if(color_code)
          {
            current_color =
             combine_colors(color_codes[current_arg + 1], bg_color);
          }

          memcpy(temp_buffer, line_pos, arg_length);
          temp_buffer[arg_length] = 0;

          if((x + arg_length) > 78)
          {
            temp_buffer[78 - x] = 0;
            if(use_mask)
            {
              write_string_mask(temp_buffer, x, y, current_color, 0);
            }
            else
            {
              write_string_ext(temp_buffer, x, y, current_color,
               0, chars_offset, 16);
            }
            break;
          }

          if(use_mask)
          {
            write_string_mask(temp_buffer, x, y, current_color, 0);
          }
          else
          {
            write_string_ext(temp_buffer, x, y, current_color,
             0, chars_offset, 16);
          }
        }

        line_pos += arg_length;
        x += arg_length;
      }
    }
  }
}

#define VALIDATE_ELEMENTS 60
#define MAX_ERRORS 256

// This will only check for errors after current_rline, so the
// base should be passed.

static int validate_lines(robot_state *rstate, int show_none)
{
  // 70x5-21 square that displays up to 13 error messages,
  // and up to 48 buttons for delete/comment out/ignore. In total,
  // the elements are text display to tell how many erroroneous
  // lines there are, 13 text bars to display the error message
  // and validity status, 13 delete buttons, 13 comment buttons,
  // 13 ignore buttons, a next button, a previous button,
  // and yes/no buttons. There can thus be up to 57 or so elements.
  // The first one is always text, and the second are always
  // yes/no buttons. The rest are variable depending on how
  // many errors there are. The text button positions cannot be
  // determined until the number of errors is.
  // Doesn't take any more errors after the number of erroneous
  // lines exceeds MAX_ERRORS.

  World *mzx_world = rstate->mzx_world;
  element *elements[VALIDATE_ELEMENTS];
  dialog di;
  char information[64] = { 0 };
  int start_line = 0;
  int num_errors = 0;
  char error_messages[MAX_ERRORS][64];
  char null_buffer[256];
  robot_line *line_pointers[MAX_ERRORS];
  robot_line *current_rline = rstate->base->next;
  validity_types validity_options[MAX_ERRORS];
  int redo = 1;
  int element_pos;
  int errors_shown;
  int multi_pages = 0;
  int line_number = 0;
  int dialog_result;
  int num_ignore = 0;
  int current_size = rstate->size;
  int i;

  // First, collect the number of errors, and process error messages
  // by calling assemble_line.

  while(current_rline != NULL)
  {
    if(current_rline->validity_status != valid)
    {
      memset(error_messages[num_errors], ' ', 64);
      sprintf(error_messages[num_errors], "%05d: ", line_number + 1);
      assemble_line(current_rline->line_text, null_buffer,
       error_messages[num_errors] + 7, NULL, NULL);
      error_messages[num_errors][strlen(error_messages[num_errors])] = ' ';
      line_pointers[num_errors] = current_rline;
      validity_options[num_errors] = current_rline->validity_status;

      if(current_rline->validity_status == invalid_uncertain)
        num_ignore++;

      num_errors++;
      if(num_errors == MAX_ERRORS)
        break;
    }
    current_rline = current_rline->next;
    line_number++;
  }

  // The button return messages are as such:
  // 0 - ok
  // -1 - cancel
  // 1 - previous
  // 2 - next
  // 100-113 - ignore line
  // 200-213 - delete line
  // 300-313 - comment line

  if(num_errors > 12)
  {
    multi_pages = 1;
  }

  if(num_ignore || show_none)
  {
    do
    {
      errors_shown = num_errors - start_line;

      if(errors_shown > 12)
        errors_shown = 12;

      elements[0] = construct_label(5, 2, information);
      elements[1] = construct_button(28, 18, "OK", 0);
      elements[2] = construct_button(38, 18, "Cancel", -1);

      for(i = 0, element_pos = 3; i < errors_shown; i++,
       element_pos += 4)
      {
        switch(validity_options[start_line + i])
        {
          case invalid_uncertain:
          {
            sprintf(error_messages[start_line + i] + 44, " (ignore)");
            break;
          }

          case invalid_discard:
          {
            sprintf(error_messages[start_line + i] + 44, " (delete)");
            break;
          }

          case invalid_comment:
          {
            sprintf(error_messages[start_line + i] + 44, "(comment)");
            break;
          }

          default:
          {
            break;
          }
        }

        elements[element_pos] = construct_label(2, i + 4,
         error_messages[start_line + i]);
        elements[element_pos + 1] =
         construct_button(56, i + 4, "I", start_line + i + 100);
        elements[element_pos + 2] =
         construct_button(60, i + 4, "D", start_line + i + 200);
        elements[element_pos + 3] =
         construct_button(64, i + 4, "C", start_line + i + 300);
      }

      if(multi_pages)
      {
        if(start_line)
        {
          elements[element_pos] =
           construct_button(5, 17, "Previous", 1);
          element_pos++;
        }

        if((start_line + 12) < num_errors)
        {
          elements[element_pos] =
           construct_button(61, 17, "Next", 2);
          element_pos++;
        }

        sprintf(information, "%d errors found; displaying %d through %d.\n",
         num_errors, start_line + 1, start_line + errors_shown);
      }
      else
      {
        if(num_errors == 1)
        {
          sprintf(information, "1 error found.");
        }
        else

        if(!num_errors)
        {
          sprintf(information, "No errors found.");
        }
        else
        {
          sprintf(information, "%d errors found.", num_errors);
        }
      }

      construct_dialog(&di, "Command Summary", 5, 2, 70, 21,
       elements, element_pos, 1);

      dialog_result = run_dialog(mzx_world, &di);
      destruct_dialog(&di);

      if(dialog_result == -1)
      {
        // Cancel - bails
        redo = 0;
      }
      else

      if(dialog_result == 0)
      {
        // Okay, update validity options
        redo = 0;
        for(i = 0; i < num_errors; i++)
        {
          (line_pointers[i])->validity_status = validity_options[i];

          if(validity_options[i] == invalid_comment)
          {
            (line_pointers[i])->line_bytecode_length =
             (line_pointers[i])->line_text_length + 5;
          }
          else
          {
            (line_pointers[i])->line_bytecode_length = 0;
            (line_pointers[i])->line_text_length = 0;
          }
        }

        rstate->size = current_size;
      }
      else

      if((dialog_result >= 100) && (dialog_result < 200))
      {
        // If the last is comment, adjust size
        if(validity_options[dialog_result - 100] == invalid_comment)
        {
          current_size -=
           (line_pointers[dialog_result - 100])->line_text_length + 5;
        }

        // Make it ignore
        validity_options[dialog_result - 100] = invalid_uncertain;
      }
      else

      if((dialog_result >= 200) && (dialog_result < 300))
      {
        // If the last is comment, adjust size
        if(validity_options[dialog_result - 200] == invalid_comment)
        {
          current_size -=
           (line_pointers[dialog_result - 200])->line_text_length + 5;
        }

        // Make it delete
        validity_options[dialog_result - 200] = invalid_discard;
      }
      else

      if((dialog_result >= 300) && (dialog_result < 400))
      {
        if(current_size +
         ((line_pointers[dialog_result - 300])->line_text_length + 5) <
         MAX_OBJ_SIZE)
        {
          // If the last is not comment, adjust size
          if(validity_options[dialog_result - 300] != invalid_comment)
          {
            current_size +=
             (line_pointers[dialog_result - 300])->line_text_length + 5;
          }

          // Make it comment
          validity_options[dialog_result - 300] = invalid_comment;
        }
      }
      else

      if(dialog_result == 1)
      {
        start_line -= 12;
      }
      else

      if(dialog_result == 2)
      {
        start_line += 12;
      }
    } while(redo);
  }

  return num_ignore;
}

static void paste_buffer(robot_state *rstate)
{
  int i;

#ifdef __WIN32__
  if(IsClipboardFormatAvailable(CF_TEXT) && OpenClipboard(NULL))
  {
    HANDLE global_memory;
    char *src_data, *src_ptr;
    char line_buffer[COMMAND_BUFFER_LEN];
    int line_length;

    rstate->command_buffer = line_buffer;

    global_memory = GetClipboardData(CF_TEXT);
    if(global_memory != NULL )
    {
      src_data = (char *)GlobalLock(global_memory);
      src_ptr = src_data;

      while(*src_ptr)
      {
        line_length = strcspn(src_ptr, "\r");
        memcpy(line_buffer, src_ptr, line_length);
        line_buffer[line_length] = 0;
        add_line(rstate);
        src_ptr += line_length;
        if(*src_ptr)
          src_ptr += 2;
      }

      GlobalUnlock(global_memory);
      CloseClipboard();
    }

    rstate->command_buffer = rstate->command_buffer_space;

    return;
  }
#elif defined(CONFIG_X11)
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);

  if(SDL_GetWMInfo(&info) && (info.subsystem == SDL_SYSWM_X11))
  {
    Display *display = info.info.x11.display;
    Window window = info.info.x11.window;
    Window owner = XGetSelectionOwner(display, XA_PRIMARY);

    if((owner != None) && (owner != window))
    {
      Atom selection_type;
      int selection_format;
      unsigned long int nbytes;
      unsigned long int overflow;
      unsigned char *src_data, *src_ptr;
      char line_buffer[COMMAND_BUFFER_LEN];
      int line_length;
      int ret_type;

      XConvertSelection(display, XA_PRIMARY, XA_STRING, None,
       owner, CurrentTime);

      info.info.x11.lock_func();
      ret_type =
       XGetWindowProperty(display, owner,
       XA_STRING, 0, INT_MAX / 4, False, AnyPropertyType, &selection_type,
       &selection_format, &nbytes, &overflow, &src_data);

      if((ret_type == Success) && ((selection_type == XA_STRING) ||
       (selection_type == XInternAtom(display, "TEXT", False)) ||
       (selection_type == XInternAtom(display, "COMPOUND_TEXT", False)) ||
       (selection_type == XInternAtom(display, "UTF8_STRING", False))))
      {
        rstate->command_buffer = line_buffer;
        src_ptr = src_data;

        while(*src_ptr)
        {
          line_length = strcspn((const char *)src_ptr, "\n");
          memcpy(line_buffer, src_ptr, line_length);
          line_buffer[line_length] = 0;
          add_line(rstate);
          src_ptr += line_length;
          if(*src_ptr)
            src_ptr++;
        }

        XFree(src_data);

        rstate->command_buffer = rstate->command_buffer_space;
        info.info.x11.unlock_func();
        return;
      }

      info.info.x11.unlock_func();
    }
  }
#endif /* CONFIG_X11 */

  if(copy_buffer)
  {
    for(i = 0; i < copy_buffer_lines; i++)
    {
      rstate->command_buffer = copy_buffer[i];
      add_line(rstate);
    }
  }

  rstate->command_buffer = rstate->command_buffer_space;
}

void robot_editor(World *mzx_world, Robot *cur_robot)
{
  int key;
  int i;
  int line_text_length, line_bytecode_length;
  int mouse_press;
  char arg_types[32];
  char str_buffer[32], max_size_buffer[32];
  char *current_robot_pos = cur_robot->program + 1;
  char *next;
  char *object_code_position;
  int first_line_draw_position;
  int first_line_count_back;
  int new_line;
  int arg_count;
  int last_color = 0;
  int last_char = 0;
  robot_line base;
  robot_line *current_rline = NULL;
  robot_line *previous_rline = &base;
  robot_line *draw_rline;
  robot_line *next_line;
  int mark_current_line;
  char text_buffer[COMMAND_BUFFER_LEN], error_buffer[COMMAND_BUFFER_LEN];
  int current_line_color;
  robot_state rstate;

  rstate.current_line = 0;
  rstate.current_rline = &base;
  rstate.total_lines = 0;
  rstate.size = 2;
  rstate.max_size = 65535;
  rstate.include_ignores = mzx_world->conf.disassemble_extras;
  rstate.disassemble_base = mzx_world->conf.disassemble_base;
  rstate.default_invalid =
   (validity_types)(mzx_world->conf.default_invalid_status);
  rstate.ccodes = mzx_world->conf.color_codes;
  rstate.mark_mode = 0;
  rstate.mark_start = -1;
  rstate.mark_end = -1;
  rstate.mark_start_rline = NULL;
  rstate.mark_end_rline = NULL;
  rstate.show_line_numbers = 0;
  rstate.color_code = mzx_world->conf.color_coding_on;
  rstate.current_x = 0;
  rstate.active_macro = NULL;
  rstate.base = &base;
  rstate.command_buffer = rstate.command_buffer_space;
  rstate.mzx_world = mzx_world;

  base.previous = NULL;
  base.line_bytecode_length = -1;

  current_line_color = combine_colors(rstate.ccodes[0], bg_color);

  // Disassemble robots into lines
  do
  {
    new_line = disassemble_line(current_robot_pos, &next,
     text_buffer, error_buffer, &line_text_length, rstate.include_ignores,
     arg_types, &arg_count, rstate.disassemble_base);

    if(new_line)
    {
      current_rline = malloc(sizeof(robot_line));

      line_bytecode_length = next - current_robot_pos;
      current_rline->line_text_length = line_text_length;
      current_rline->line_bytecode_length = line_bytecode_length;
      current_rline->line_text = malloc(line_text_length + 1);
      current_rline->line_bytecode = malloc(line_bytecode_length);
      current_rline->num_args = arg_count;
      current_rline->validity_status = valid;

      memcpy(current_rline->arg_types, arg_types, 20);
      memcpy(current_rline->line_bytecode, current_robot_pos,
       line_bytecode_length);
      memcpy(current_rline->line_text, text_buffer, line_text_length + 1);

      previous_rline->next = current_rline;
      current_rline->previous = previous_rline;

      rstate.total_lines++;

      current_robot_pos = next;
      previous_rline = current_rline;

      rstate.size += line_bytecode_length;
    }
    else
    {
      previous_rline->next = NULL;
    }
  } while(new_line);

  // Add a blank line to the end too if there aren't any lines
  if(!rstate.total_lines)
  {
    add_blank_line(&rstate, 1);
  }
  else
  {
    move_line_down(&rstate, 1);
  }

  save_screen();
  fill_line(80, 0, 0, top_char, top_color);

  if(mzx_world->conf.redit_hhelp)
  {
    rstate.scr_hide_mode = 1;
    write_string(key_help_hide, 0, 24, bottom_text_color, 0);
    rstate.scr_line_start = 1;
    rstate.scr_line_middle = 12;
    rstate.scr_line_end = 23;
  }
  else
  {
    rstate.scr_hide_mode = 0;
    write_string(key_help, 0, 22, bottom_text_color, 0);
    rstate.scr_line_start = 2;
    rstate.scr_line_middle = 11;
    rstate.scr_line_end = 20;
  }

  sprintf(max_size_buffer, "%05d", rstate.max_size);
  strcpy(rstate.command_buffer, rstate.current_rline->line_text);

  do
  {
    draw_char(top_line_connect, line_color, 0, 1);
    draw_char(top_line_connect, line_color, 79, 1);
    draw_char(bottom_line_connect, line_color, 0, 21);
    draw_char(bottom_line_connect, line_color, 79, 21);

    for(i = 1; i < 79; i++)
    {
      draw_char(horizontal_line, line_color, i, 1);
      draw_char(horizontal_line, line_color, i, 21);
    }

    for(i = rstate.scr_line_start; i < rstate.scr_line_middle; i++)
    {
      draw_char(vertical_line, line_color, 0, i);
      draw_char(vertical_line, line_color, 79, i);
      fill_line(78, 1, i, bg_char, bg_color_solid);
    }

    for(i++; i <= rstate.scr_line_end; i++)
    {
      draw_char(vertical_line, line_color, 0, i);
      draw_char(vertical_line, line_color, 79, i);
      fill_line(78, 1, i, bg_char, bg_color_solid);
    }

    write_string("Line:", 1, 0, top_highlight_color, 0);
    sprintf(str_buffer, "%05d", rstate.current_line);
    write_string(str_buffer, 7, 0, top_text_color, 0);
    write_string("/", 12, 0, top_highlight_color, 0);
    sprintf(str_buffer, "%05d", rstate.total_lines);
    write_string(str_buffer, 13, 0, top_text_color, 0);
    write_string("Character:", 26, 0, top_highlight_color, 0);
    write_string("/", 40, 0, top_highlight_color, 0);
    write_string("Size:", 51, 0, top_highlight_color, 0);
    sprintf(str_buffer, "%05d", rstate.size);
    write_string(str_buffer, 57, 0, top_text_color, 0);
    write_string("/", 62, 0, top_highlight_color, 0);
    write_string(max_size_buffer, 63, 0, top_text_color, 0);

    // Now, draw the lines. Start with 9 back from the current.

    if(rstate.current_line >
      (rstate.scr_line_middle - rstate.scr_line_start))
    {
      first_line_draw_position = rstate.scr_line_start;
      first_line_count_back =
       rstate.scr_line_middle - rstate.scr_line_start;
    }
    else
    {
      // Or go back as many as we can
      first_line_draw_position = 1 +
       (rstate.scr_line_middle - rstate.current_line);
      first_line_count_back = rstate.current_line - 1;
    }

    draw_rline = rstate.current_rline;
    for(i = 0; i < first_line_count_back; i++)
    {
      draw_rline = draw_rline->previous;
    }

    // Now draw start - scr_end + 1 lines, or until bails
    for(i = first_line_draw_position;
     (i <= rstate.scr_line_end) && draw_rline; i++)
    {
      if(i != rstate.scr_line_middle)
        robo_ed_display_robot_line(&rstate, draw_rline, i);

      draw_rline = draw_rline->next;
    }

    // Mark block selection area
    mark_current_line = 0;
    if(rstate.mark_mode)
    {
      int draw_start = rstate.current_line -
       (rstate.scr_line_middle - rstate.scr_line_start);
      int draw_end = rstate.current_line +
       (rstate.scr_line_end - rstate.scr_line_middle) + 1;

      if(rstate.mark_start <= draw_end)
      {
        if(rstate.mark_start >= draw_start)
        {
          draw_start = rstate.mark_start +
           rstate.scr_line_middle - rstate.current_line;
          draw_char('S', mark_color, 0, draw_start);
        }
        else
        {
          draw_start += rstate.scr_line_middle - rstate.current_line;
        }

        if(rstate.mark_end <= draw_end)
        {
          draw_end = rstate.mark_end + rstate.scr_line_middle -
          rstate.current_line;
        }
        else
        {
          draw_end += rstate.scr_line_middle - rstate.current_line;
        }

        if(draw_start < rstate.scr_line_start)
        {
          draw_start = rstate.scr_line_start;
        }

        if(draw_end > rstate.scr_line_end)
        {
          draw_end = rstate.scr_line_end;
        }
        else
        {
          if((rstate.mark_start != rstate.mark_end) &&
           (draw_end >= rstate.scr_line_start))
            draw_char('E', mark_color, 0, draw_end);
        }

        if(draw_end >= rstate.scr_line_start)
        {
          for(i = draw_start; i <= draw_end; i++)
          {
            color_line(80, 0, i, mark_color);
          }

          if((draw_start <= rstate.scr_line_middle) &&
           (draw_end >= rstate.scr_line_middle))
            mark_current_line = 1;
        }
      }
    }

    rstate.macro_repeat_level = 0;

    update_screen();

    if(mark_current_line)
    {
      draw_char(bg_char, mark_color, 1, rstate.scr_line_middle);
      key = intake(mzx_world, rstate.command_buffer, 240, 2,
       rstate.scr_line_middle, mark_color, 2, 0, &rstate.current_x,
       1, rstate.active_macro);
    }
    else
    {
      draw_char(bg_char, bg_color_solid, 1, rstate.scr_line_middle);
      key = intake(mzx_world, rstate.command_buffer, 240, 2,
       rstate.scr_line_middle, current_line_color, 2, 0,
       &rstate.current_x, 1, rstate.active_macro);
    }

    mouse_press = get_mouse_press_ext();

    if(mouse_press && (mouse_press <= MOUSE_BUTTON_RIGHT))
    {
      int mouse_x, mouse_y;
      get_mouse_position(&mouse_x, &mouse_y);

      if((mouse_y >= rstate.scr_line_start) &&
       (mouse_y <= rstate.scr_line_end) && (mouse_x >= 2) &&
       (mouse_x <= 78))
      {
        move_and_update(&rstate, mouse_y - rstate.scr_line_middle);
        rstate.current_x = mouse_x - 2;
        warp_mouse(mouse_x, rstate.scr_line_middle);
      }
    }
    else

    if(mouse_press == MOUSE_BUTTON_WHEELUP)
    {
      move_and_update(&rstate, -3);
    }
    else

    if(mouse_press == MOUSE_BUTTON_WHEELDOWN)
    {
      move_and_update(&rstate, 3);
    }

    rstate.active_macro = NULL;
    rstate.macro_recurse_level = 0;

    switch(key)
    {
      case IKEY_UP:
      {
        move_and_update(&rstate, -1);
        break;
      }

      case IKEY_DOWN:
      {
        move_and_update(&rstate, 1);
        break;
      }

      case IKEY_PAGEUP:
      {
        move_and_update(&rstate, -9);
        break;
      }

      case IKEY_PAGEDOWN:
      {
        move_and_update(&rstate, 9);
        break;
      }

      case IKEY_BACKSPACE:
      {
        if(rstate.current_x == 0)
        {
          rstate.current_x = 10000;
          if(rstate.command_buffer[0] == 0)
          {
            update_current_line(&rstate);
            delete_current_line(&rstate, -1);
          }
          else
          {
            move_and_update(&rstate, -1);
          }
        }
        break;
      }

      case IKEY_DELETE:
      {

        if(rstate.command_buffer[0] == 0)
        {
          update_current_line(&rstate);
          delete_current_line(&rstate, 1);
        }
        break;
      }

      case IKEY_RETURN:
      {
        if(get_alt_status(keycode_internal))
        {
          block_action(&rstate);
        }
        else

        if(rstate.current_x)
        {
          move_and_update(&rstate, 1);
          rstate.current_x = 0;
        }
        else
        {
          add_blank_line(&rstate, -1);
        }
        break;
      }

#ifdef CONFIG_HELPSYS
      case IKEY_F1:
      {
        m_show();
        help_system(mzx_world);
        break;
      }
#endif

      case IKEY_F2:
      {
        int new_color;

        new_color = color_selection(last_color, 1);
        if(new_color >= 0)
        {
          char color_buffer[16];
          last_color = new_color;
          print_color(new_color, color_buffer);
          insert_string(rstate.command_buffer, color_buffer, &rstate.current_x);
        }

        break;
      }

      case IKEY_F3:
      {
        int new_char = char_selection(last_char);

        if(new_char >= 0)
        {
          char char_buffer[16];
          int chars_length = unescape_char(char_buffer, new_char);
          char_buffer[chars_length] = 0;

          insert_string(rstate.command_buffer, char_buffer, &rstate.current_x);

          last_char = new_char;
        }
        break;
      }

      case IKEY_F4:
      {
        int start_x = rstate.current_x;
        const search_entry_short *matched_arg;
        int end_x;
        char temp_char;

        if(!rstate.command_buffer[start_x])
          start_x--;

        while(start_x && (rstate.command_buffer[start_x] == ' '))
          start_x--;

        end_x = start_x + 1;
        temp_char = rstate.command_buffer[end_x];

        while(start_x && (rstate.command_buffer[start_x] != ' '))
          start_x--;

        if(start_x)
          start_x++;

        temp_char = rstate.command_buffer[end_x];
        rstate.command_buffer[end_x] = 0;
        matched_arg = find_argument(rstate.command_buffer + start_x);
        rstate.command_buffer[end_x] = temp_char;

        if(matched_arg && (matched_arg->type == S_THING) &&
         (matched_arg->offset < 122))
        {
          int new_param = edit_param(mzx_world, matched_arg->offset, -1);
          if(new_param >= 0)
          {
            char param_buffer[16];
            sprintf(param_buffer, " p%02x", new_param);
            insert_string(rstate.command_buffer, param_buffer,
             &rstate.current_x);
          }
        }
        break;
      }

      case IKEY_F5:
      {
        int char_edited;
        char char_buffer[14];
        char char_string_buffer[64];
        char *char_string_buffer_position = char_string_buffer;

        char_edited = char_editor(mzx_world);

        ec_read_char(char_edited, char_buffer);

        for(i = 0; i < 13; i++, char_string_buffer_position += 4)
        {
          sprintf(char_string_buffer_position, "$%02x ", char_buffer[i]);
        }
        sprintf(char_string_buffer_position, "$%02x", char_buffer[i]);

        insert_string(rstate.command_buffer, char_string_buffer,
         &rstate.current_x);
        break;
      }

      case IKEY_F6:
      {
        execute_numbered_macro(&rstate, 1);
        break;
      }

      case IKEY_F7:
      {
        execute_numbered_macro(&rstate, 2);
        break;
      }

      case IKEY_F8:
      {
        execute_numbered_macro(&rstate, 3);
        break;
      }

      case IKEY_F9:
      {
        execute_numbered_macro(&rstate, 4);
        break;
      }

      case IKEY_F10:
      {
        execute_numbered_macro(&rstate, 5);
        break;
      }

      case IKEY_v:
      {
        if(get_alt_status(keycode_internal))
        {
          update_current_line(&rstate);
          validate_lines(&rstate, 1);
          update_current_line(&rstate);
        }
        break;
      }

      case IKEY_ESCAPE:
      {
        update_current_line(&rstate);
        if(validate_lines(&rstate, 0))
          key = 0;

        update_current_line(&rstate);
        break;
      }

      // Mark start/end
      case IKEY_s:
      case IKEY_e:
      case IKEY_HOME:
      case IKEY_END:
      {
        if(get_alt_status(keycode_internal))
        {
          int mark_switch;

          update_current_line(&rstate);

          if(rstate.mark_mode == 2)
          {
            if((key == IKEY_HOME) || (key == IKEY_s))
              mark_switch = 0;
            else
              mark_switch = 1;
          }
          else
          {
            mark_switch = rstate.mark_mode;
            rstate.mark_mode++;
          }

          switch(mark_switch)
          {
            case 0:
            {
              // Always mark the start
              rstate.mark_start = rstate.current_line;
              rstate.mark_start_rline = rstate.current_rline;
              if(rstate.mark_mode == 1)
              {
                // If this is the real start, mark the end too
                rstate.mark_end = rstate.current_line;
                rstate.mark_end_rline = rstate.current_rline;
              }
              break;
            }

            case 1:
            {
              // Always mark the end
              rstate.mark_end = rstate.current_line;
              rstate.mark_end_rline = rstate.current_rline;
              break;
            }
          }

          // We have more than a start, so possibly swap
          if(rstate.mark_mode && (rstate.mark_start > rstate.mark_end))
          {
            int mark_swap;
            robot_line *mark_swap_rline;

            mark_swap = rstate.mark_start;
            rstate.mark_start = rstate.mark_end;
            rstate.mark_end = mark_swap;
            mark_swap_rline = rstate.mark_start_rline;
            rstate.mark_start_rline = rstate.mark_end_rline;
            rstate.mark_end_rline = mark_swap_rline;
          }
        }
        break;
      }

      // Block action menu
      case IKEY_b:
      {
        if(get_alt_status(keycode_internal))
        {
          block_action(&rstate);
        }
        break;
      }

      case IKEY_g:
      {
        if(get_ctrl_status(keycode_internal))
          goto_position(mzx_world, &rstate);

        break;
      }

      case IKEY_l:
      {
        if(get_ctrl_status(keycode_internal))
          rstate.show_line_numbers ^= 1;

        break;
      }

      case IKEY_d:
      {
        if(rstate.current_rline->validity_status != valid)
        {
          rstate.current_rline->validity_status = invalid_discard;
          update_current_line(&rstate);
        }

        break;
      }

      case IKEY_c:
      {
        if(rstate.current_rline->validity_status != valid)
        {
          rstate.current_rline->validity_status = invalid_comment;
          update_current_line(&rstate);
        }
        else

        if(rstate.command_buffer[0] != '.')
        {
          char comment_buffer[COMMAND_BUFFER_LEN];
          char current_char;
          char *in_position = rstate.command_buffer;
          char *out_position = comment_buffer + 3;

          comment_buffer[0] = '.';
          comment_buffer[1] = ' ';
          comment_buffer[2] = '"';

          do
          {
            current_char = *in_position;
            if((current_char == '\"') || (current_char == '\\'))
            {
              *out_position = '\\';
              out_position++;
            }

            *out_position = current_char;
            out_position++;
            in_position++;
          }
          while(out_position - comment_buffer < 240 && current_char);

          *(out_position - 1) = '"';
          *out_position = 0;

          strcpy(rstate.command_buffer, comment_buffer);
        }
        else

        if(get_ctrl_status(keycode_internal) &&
         (rstate.command_buffer[0] == '.') &&
         (rstate.command_buffer[1] == ' ') &&
         (rstate.command_buffer[2] == '"') &&
         (rstate.command_buffer[strlen(rstate.command_buffer) - 1] == '"'))
        {
          char uncomment_buffer[COMMAND_BUFFER_LEN];
          char current_char;
          char *in_position = rstate.command_buffer + 3;
          char *out_position = uncomment_buffer;

          do
          {
            current_char = *in_position;
            if((current_char == '\\') && (in_position[1] == '"'))
            {
              current_char = '"';
              in_position++;
            }

            if((current_char == '\\') && (in_position[1] == '\\'))
            {
              current_char = '\\';
              in_position++;
            }


            *out_position = current_char;
            out_position++;
            in_position++;
          } while(current_char);

          *(out_position - 2) = 0;

          strcpy(rstate.command_buffer, uncomment_buffer);
        }

        break;
      }

      case IKEY_p:
      case IKEY_INSERT:
      {
        if(get_alt_status(keycode_internal))
        {
          paste_buffer(&rstate);
        }
        break;
      }

      // Import file
      case IKEY_i:
      {
        if(get_alt_status(keycode_internal))
        {
          import_block(mzx_world, &rstate);
        }
        else

        if(get_ctrl_status(keycode_internal))
        {
          if(rstate.current_rline->validity_status != valid)
          {
            rstate.current_rline->validity_status = invalid_uncertain;
          }
        }
        break;
      }

      // Export file
      case IKEY_x:
      {
        if(get_alt_status(keycode_internal))
        {
          if(rstate.mark_mode)
          {
            export_block(&rstate, 1);
          }
          else
          {
            export_block(&rstate, 0);
          }
        }
        break;
      }

      // Options
      case IKEY_o:
      {
        if(get_alt_status(keycode_internal))
          edit_settings(mzx_world);

        break;
      }

      // Unmark
      case IKEY_u:
      {
        if(get_alt_status(keycode_internal))
          rstate.mark_mode = 0;

        break;
      }

      // Hide
      case IKEY_h:
      {
        if(get_alt_status(keycode_internal))
        {
          if(rstate.scr_hide_mode)
          {
            rstate.scr_hide_mode = 0;
            rstate.scr_line_start = 2;
            rstate.scr_line_middle = 11;
            rstate.scr_line_end = 20;
            write_string(key_help, 0, 22, bottom_text_color, 0);
          }
          else
          {
            rstate.scr_hide_mode = 1;
            rstate.scr_line_start = 1;
            rstate.scr_line_middle = 12;
            rstate.scr_line_end = 23;
            write_string(key_help_hide, 0, 24, bottom_text_color, 0);
          }
        }
        break;
      }

      case IKEY_f:
      {
        if(get_ctrl_status(keycode_internal))
          find_replace_action(&rstate);

        break;
      }

      case IKEY_r:
      {
        if(get_ctrl_status(keycode_internal))
        {
          switch(last_find_option)
          {
            case 0:
            {
              // Find next
              int l_pos;
              int l_num = robo_ed_find_string(&rstate, search_string,
               wrap_option, &l_pos, case_option);

              if(l_num != -1)
              {
                goto_line(&rstate, l_num);
                rstate.current_x = l_pos;
              }
              break;
            }

            case 1:
            case 2:
            {
              // Find and replace next
              int l_pos;
              int l_num = robo_ed_find_string(&rstate, search_string,
               wrap_option, &l_pos, case_option);

              if(l_num != -1)
              {
                goto_line(&rstate, l_num);
                rstate.current_x = l_pos;
                replace_current_line(&rstate, l_pos, search_string,
                 replace_string);
              }
              break;
            }
          }
        }
        break;
      }

      case IKEY_m:
      {
        if(get_alt_status(keycode_internal))
        {
          char macro_line[256];

          macro_line[0] = 0;

          save_screen();
          draw_window_box(15, 11, 65, 13, EC_DEBUG_BOX, EC_DEBUG_BOX_DARK,
           EC_DEBUG_BOX_CORNER, 1, 1);
          write_string("Configure macro:", 17, 12, EC_DEBUG_LABEL, 0);

          if(intake(mzx_world, macro_line, 29, 34, 12, 15, 1, 0, NULL,
           0, NULL) != IKEY_ESCAPE)
          {
            ext_macro *macro_src;
            int next;

            restore_screen();
            macro_src = find_macro(&(mzx_world->conf), macro_line, &next);

            if(macro_src)
              execute_macro(&rstate, macro_src);
          }
          else
          {
            restore_screen();
          }
        }
        break;
      }
    }
  } while(key != IKEY_ESCAPE);

  // Package time
  reallocate_robot(cur_robot, rstate.size);

  current_rline = base.next;
  object_code_position = cur_robot->program;
  object_code_position[0] = 0xFF;
  object_code_position++;

  while(current_rline)
  {
    if((current_rline->next != NULL) || (current_rline->line_text[0]))
    {
      if(current_rline->validity_status == invalid_comment)
      {
        object_code_position[0] = current_rline->line_text_length + 3;
        object_code_position[1] = 107;
        object_code_position[2] = current_rline->line_text_length + 1;

        strcpy(object_code_position + 3, current_rline->line_text);
        object_code_position[current_rline->line_text_length + 4] =
         current_rline->line_text_length + 3;
        object_code_position += current_rline->line_text_length + 5;
      }
      else

      if(current_rline->validity_status == valid)
      {
        memcpy(object_code_position, current_rline->line_bytecode,
         current_rline->line_bytecode_length);
        object_code_position += current_rline->line_bytecode_length;
      }
    }
    else
    {
      // Get rid of trailing three bytes if present
      rstate.size -= current_rline->line_bytecode_length;
      reallocate_robot(cur_robot, rstate.size);
      object_code_position = cur_robot->program + rstate.size - 1;
    }

    free(current_rline->line_bytecode);
    free(current_rline->line_text);
    next_line = current_rline->next;
    free(current_rline);
    current_rline = next_line;
  }

  object_code_position[0] = 0;

  if(rstate.size > 2)
  {
    cur_robot->used = 1;
    cur_robot->cur_prog_line = 1;
    cur_robot->label_list = cache_robot_labels(cur_robot,
     &(cur_robot->num_labels));
  }

  restore_screen();
}