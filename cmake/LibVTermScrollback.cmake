# libvterm 0.3.3 drops continuation metadata when rows cross the scrollback boundary.
# Build private copies of the affected sources with callbacks that carry it in both directions.
function(terminal_vterm_patch variable before after)
    string(FIND "${${variable}}" "${before}" position)
    if (position EQUAL -1)
        message(FATAL_ERROR "The Lib-Terminal scrollback patch no longer matches libvterm's ${variable}")
    endif ()
    string(REPLACE "${before}" "${after}" patched "${${variable}}")
    set(${variable} "${patched}" PARENT_SCOPE)
endfunction()

set(terminal_vterm_patched_dir "${CMAKE_CURRENT_BINARY_DIR}/terminal-vterm")
foreach (file IN ITEMS include/vterm.h src/state.c src/screen.c src/vterm_internal.h)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${terminal_vterm_SOURCE_DIR}/${file}")
endforeach ()
file(READ "${terminal_vterm_SOURCE_DIR}/include/vterm.h" header)
file(READ "${terminal_vterm_SOURCE_DIR}/src/vterm_internal.h" internal)
file(READ "${terminal_vterm_SOURCE_DIR}/src/state.c" state)
file(READ "${terminal_vterm_SOURCE_DIR}/src/screen.c" screen)

terminal_vterm_patch(header
    "(*sb_pushline)(int cols, const VTermScreenCell *cells, void *user)"
    "(*sb_pushline)(int cols, const VTermScreenCell *cells, bool continuation, void *user)")
terminal_vterm_patch(header
    "(*sb_popline)(int cols, VTermScreenCell *cells, void *user)"
    "(*sb_popline)(int cols, VTermScreenCell *cells, bool *continuation, void *user)")
terminal_vterm_patch(internal "void vterm_screen_free(VTermScreen *screen);"
    "void vterm_screen_free(VTermScreen *screen);\nvoid vterm_screen_push_scrolled_rows(VTermScreen *screen, int rows);")
terminal_vterm_patch(state "  // Update lineinfo if full line" [=[
  if(state->vt->screen && rect.start_row == 0 && rect.start_col == 0 &&
     rect.end_col == state->cols && rightward == 0 && downward > 0)
    vterm_screen_push_scrolled_rows(state->vt->screen, downward);

  // Update lineinfo if full line]=])

terminal_vterm_patch(screen "static void sb_pushline_from_row(VTermScreen *screen, int row)"
    "static void sb_pushline_from_row(VTermScreen *screen, int row, bool continuation)")
terminal_vterm_patch(screen
    "(screen->callbacks->sb_pushline)(screen->cols, screen->sb_buffer, screen->cbdata);"
    "(screen->callbacks->sb_pushline)(screen->cols, screen->sb_buffer, continuation, screen->cbdata);")
terminal_vterm_patch(screen [=[
static int moverect_internal(VTermRect dest, VTermRect src, void *user)
{
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->sb_pushline &&
     dest.start_row == 0 && dest.start_col == 0 &&        // starts top-left corner
     dest.end_col == screen->cols &&                      // full width
     screen->buffer == screen->buffers[BUFIDX_PRIMARY]) { // not altscreen
    for(int row = 0; row < src.start_row; row++)
      sb_pushline_from_row(screen, row);
  }
]=] [=[
INTERNAL void vterm_screen_push_scrolled_rows(VTermScreen *screen, int rows)
{
  if(screen->callbacks && screen->callbacks->sb_pushline &&
     screen->buffer == screen->buffers[BUFIDX_PRIMARY]) {
    for(int row = 0; row < rows; row++)
      sb_pushline_from_row(screen, row, vterm_state_get_lineinfo(screen->state, row)->continuation);
  }
}

static int moverect_internal(VTermRect dest, VTermRect src, void *user)
{
  VTermScreen *screen = user;
]=])
terminal_vterm_patch(screen "old_row >= 0 && old_lineinfo[old_row].continuation"
    "old_row > 0 && old_lineinfo[old_row].continuation")
terminal_vterm_patch(screen "new_lineinfo[new_row].continuation = (new_row > new_row_start);"
    "new_lineinfo[new_row].continuation = (new_row > new_row_start) || old_lineinfo[old_row_start].continuation;")
terminal_vterm_patch(screen [=[
    if(screen->callbacks && screen->callbacks->sb_pushline)
      for(int row = 0; row <= old_row; row++)
        sb_pushline_from_row(screen, row);
]=] [=[
    if(screen->callbacks && screen->callbacks->sb_pushline) {
      ScreenCell *active_buffer = screen->buffer;
      screen->buffer = old_buffer;
      for(int row = 0; row <= old_row; row++)
        sb_pushline_from_row(screen, row, old_lineinfo[row].continuation);
      screen->buffer = active_buffer;
    }
]=])
terminal_vterm_patch(screen [=[
      if(!(screen->callbacks->sb_popline(old_cols, screen->sb_buffer, screen->cbdata)))
        break;

      VTermPos pos = { .row = new_row };
]=] [=[
      bool continuation = false;
      if(!(screen->callbacks->sb_popline(old_cols, screen->sb_buffer, &continuation, screen->cbdata)))
        break;

      new_lineinfo[new_row] = (VTermLineInfo){ .continuation = continuation };
      VTermPos pos = { .row = new_row };
]=])

file(CONFIGURE OUTPUT "${terminal_vterm_patched_dir}/vterm.h" CONTENT "@header@" @ONLY)
file(CONFIGURE OUTPUT "${terminal_vterm_patched_dir}/vterm_internal.h" CONTENT "@internal@" @ONLY)
file(CONFIGURE OUTPUT "${terminal_vterm_patched_dir}/state.c" CONTENT "@state@" @ONLY)
file(CONFIGURE OUTPUT "${terminal_vterm_patched_dir}/screen.c" CONTENT "@screen@" @ONLY)
