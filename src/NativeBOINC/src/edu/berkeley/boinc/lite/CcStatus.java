/* 
 * AndroBOINC - BOINC Manager for Android
 * Copyright (C) 2010, Pavol Michalec
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

package edu.berkeley.boinc.lite;


public class CcStatus {
	// all attributes are public for simple access
	/** Current mode - always/auto/never */
	public int     task_mode           	  = -1;
	/** Permanent version of mode - always/auto/never */
	public int     task_mode_perm         = -1;
	/** time until task_mode_perm becomes actual task_mode */
	public double  task_mode_delay        =  0;
	public int     task_suspend_reason    = -1;
	public int     network_mode           = -1;
	public int     network_mode_perm      = -1;
	public double  network_mode_delay     =  0;
	public int     network_suspend_reason = -1;
	public int     network_status         = -1;
	public int     gpu_mode           	  = -1;
	public int     gpu_mode_perm          = -1;
	public double  gpu_mode_delay         =  0;
	public int     gpu_suspend_reason     = -1;
	public boolean ams_password_error = false;
	public boolean manager_must_quit  = false;
	public boolean disallow_attach    = false;
	public boolean simple_gui_only    = false;
}
