/* 
 * NativeBOINC - Native BOINC Client with Manager
 * Copyright (C) 2011, Mateusz Szpakowski
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

package sk.boinc.nativeboinc;

import sk.boinc.nativeboinc.util.ScreenOrientationHandler;
import android.app.Activity;
import android.os.Bundle;

/**
 * @author mat
 *
 */
public class AbstractBoincActivity extends Activity {
	
	private ScreenOrientationHandler mScreenOrientation;
	
	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		mScreenOrientation = new ScreenOrientationHandler(this);
	}

	@Override
	protected void onResume() {
		super.onResume();
		mScreenOrientation.setOrientation();
	}
	
	@Override
	protected void onDestroy() {
		mScreenOrientation = null;
		super.onDestroy();
	}
}
