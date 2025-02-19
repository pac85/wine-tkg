/*
 * Window messaging support
 *
 * Copyright 2001 Alexandre Julliard
 * Copyright 2008 Maarten Lankhorst
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */


#if 0
#pragma makedep unix
#endif

#include "win32u_private.h"
#include "ntuser_private.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(msg);


/***********************************************************************
 *           handle_internal_message
 *
 * Handle an internal Wine message instead of calling the window proc.
 */
LRESULT handle_internal_message( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch(msg)
    {
    case WM_WINE_DESTROYWINDOW:
        return destroy_window( hwnd );
    case WM_WINE_SETWINDOWPOS:
        if (is_desktop_window( hwnd )) return 0;
        return set_window_pos( (WINDOWPOS *)lparam, 0, 0 );
    case WM_WINE_SHOWWINDOW:
        if (is_desktop_window( hwnd )) return 0;
        return NtUserShowWindow( hwnd, wparam );
    case WM_WINE_SETPARENT:
        if (is_desktop_window( hwnd )) return 0;
        return HandleToUlong( NtUserSetParent( hwnd, UlongToHandle(wparam) ));
    case WM_WINE_SETWINDOWLONG:
        return set_window_long( hwnd, (short)LOWORD(wparam), HIWORD(wparam), lparam, FALSE );
    case WM_WINE_SETSTYLE:
        if (is_desktop_window( hwnd )) return 0;
        return set_window_style( hwnd, wparam, lparam );
    case WM_WINE_SETACTIVEWINDOW:
        if (!wparam && NtUserGetForegroundWindow() == hwnd) return 0;
        return (LRESULT)NtUserSetActiveWindow( (HWND)wparam );
    case WM_WINE_KEYBOARD_LL_HOOK:
    case WM_WINE_MOUSE_LL_HOOK:
    {
        struct hook_extra_info *h_extra = (struct hook_extra_info *)lparam;

        return call_current_hook( h_extra->handle, HC_ACTION, wparam, h_extra->lparam );
    }
    case WM_WINE_CLIPCURSOR:
        if (wparam)
        {
            RECT rect;
            get_clip_cursor( &rect );
            return user_driver->pClipCursor( &rect );
        }
        return user_driver->pClipCursor( NULL );
    case WM_WINE_UPDATEWINDOWSTATE:
        update_window_state( hwnd );
        return 0;
    default:
        if (msg >= WM_WINE_FIRST_DRIVER_MSG && msg <= WM_WINE_LAST_DRIVER_MSG)
            return user_driver->pWindowMessage( hwnd, msg, wparam, lparam );
        FIXME( "unknown internal message %x\n", msg );
        return 0;
    }
}

/***********************************************************************
 *           NtUserWaitForInputIdle (win32u.@)
 */
DWORD WINAPI NtUserWaitForInputIdle( HANDLE process, DWORD timeout, BOOL wow )
{
    if (!user_callbacks) return 0;
    return user_callbacks->pWaitForInputIdle( process, timeout );
}

/**********************************************************************
 *	     NtUserGetGUIThreadInfo  (win32u.@)
 */
BOOL WINAPI NtUserGetGUIThreadInfo( DWORD id, GUITHREADINFO *info )
{
    BOOL ret;

    if (info->cbSize != sizeof(*info))
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    SERVER_START_REQ( get_thread_input )
    {
        req->tid = id;
        if ((ret = !wine_server_call_err( req )))
        {
            info->flags          = 0;
            info->hwndActive     = wine_server_ptr_handle( reply->active );
            info->hwndFocus      = wine_server_ptr_handle( reply->focus );
            info->hwndCapture    = wine_server_ptr_handle( reply->capture );
            info->hwndMenuOwner  = wine_server_ptr_handle( reply->menu_owner );
            info->hwndMoveSize   = wine_server_ptr_handle( reply->move_size );
            info->hwndCaret      = wine_server_ptr_handle( reply->caret );
            info->rcCaret.left   = reply->rect.left;
            info->rcCaret.top    = reply->rect.top;
            info->rcCaret.right  = reply->rect.right;
            info->rcCaret.bottom = reply->rect.bottom;
            if (reply->menu_owner) info->flags |= GUI_INMENUMODE;
            if (reply->move_size) info->flags |= GUI_INMOVESIZE;
            if (reply->caret) info->flags |= GUI_CARETBLINKING;
        }
    }
    SERVER_END_REQ;
    return ret;
}

/***********************************************************************
 *           NtUserSetTimer (win32u.@)
 */
UINT_PTR WINAPI NtUserSetTimer( HWND hwnd, UINT_PTR id, UINT timeout, TIMERPROC proc, ULONG tolerance )
{
    UINT_PTR ret;
    WNDPROC winproc = 0;

    if (proc) winproc = alloc_winproc( (WNDPROC)proc, TRUE );

    timeout = min( max( USER_TIMER_MINIMUM, timeout ), USER_TIMER_MAXIMUM );

    SERVER_START_REQ( set_win_timer )
    {
        req->win    = wine_server_user_handle( hwnd );
        req->msg    = WM_TIMER;
        req->id     = id;
        req->rate   = timeout;
        req->lparam = (ULONG_PTR)winproc;
        if (!wine_server_call_err( req ))
        {
            ret = reply->id;
            if (!ret) ret = TRUE;
        }
        else ret = 0;
    }
    SERVER_END_REQ;

    TRACE( "Added %p %lx %p timeout %d\n", hwnd, id, winproc, timeout );
    return ret;
}

/***********************************************************************
 *           NtUserSetSystemTimer (win32u.@)
 */
UINT_PTR WINAPI NtUserSetSystemTimer( HWND hwnd, UINT_PTR id, UINT timeout, TIMERPROC proc )
{
    UINT_PTR ret;
    WNDPROC winproc = 0;

    if (proc) winproc = alloc_winproc( (WNDPROC)proc, TRUE );

    timeout = min( max( 5, timeout ), USER_TIMER_MAXIMUM );

    SERVER_START_REQ( set_win_timer )
    {
        req->win    = wine_server_user_handle( hwnd );
        req->msg    = WM_SYSTIMER;
        req->id     = id;
        req->rate   = timeout;
        req->lparam = (ULONG_PTR)winproc;
        if (!wine_server_call_err( req ))
        {
            ret = reply->id;
            if (!ret) ret = TRUE;
        }
        else ret = 0;
    }
    SERVER_END_REQ;

    TRACE( "Added %p %lx %p timeout %d\n", hwnd, id, winproc, timeout );
    return ret;
}

/***********************************************************************
 *           NtUserKillTimer (win32u.@)
 */
BOOL WINAPI NtUserKillTimer( HWND hwnd, UINT_PTR id )
{
    BOOL ret;

    SERVER_START_REQ( kill_win_timer )
    {
        req->win = wine_server_user_handle( hwnd );
        req->msg = WM_TIMER;
        req->id  = id;
        ret = !wine_server_call_err( req );
    }
    SERVER_END_REQ;
    return ret;
}

/* see KillSystemTimer */
BOOL kill_system_timer( HWND hwnd, UINT_PTR id )
{
    BOOL ret;

    SERVER_START_REQ( kill_win_timer )
    {
        req->win = wine_server_user_handle( hwnd );
        req->msg = WM_SYSTIMER;
        req->id  = id;
        ret = !wine_server_call_err( req );
    }
    SERVER_END_REQ;
    return ret;
}

/* see SendMessageW */
LRESULT send_message( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    /* FIXME: move implementation from user32 */
    if (!user_callbacks) return 0;
    return user_callbacks->pSendMessageW( hwnd, msg, wparam, lparam );
}

/* see SendNotifyMessageW */
static BOOL send_notify_message( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, BOOL ansi )
{
    return user_callbacks && user_callbacks->pSendNotifyMessageW( hwnd, msg, wparam, lparam );
}

/* see PostMessageW */
LRESULT post_message( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    /* FIXME: move implementation from user32 */
    if (!user_callbacks) return 0;
    return user_callbacks->pPostMessageW( hwnd, msg, wparam, lparam );
}

BOOL WINAPI NtUserMessageCall( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                               ULONG_PTR result_info, DWORD type, BOOL ansi )
{
    switch (type)
    {
    case FNID_SENDNOTIFYMESSAGE:
        return send_notify_message( hwnd, msg, wparam, lparam, ansi );
    default:
        FIXME( "%p %x %lx %lx %lx %x %x\n", hwnd, msg, wparam, lparam, result_info, type, ansi );
    }
    return 0;
}
