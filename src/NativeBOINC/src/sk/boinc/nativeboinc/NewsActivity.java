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

import java.util.ArrayList;

import sk.boinc.nativeboinc.debug.Logging;
import sk.boinc.nativeboinc.news.NewsFetcherListener;
import sk.boinc.nativeboinc.news.NewsMessage;
import sk.boinc.nativeboinc.news.NewsReceiver;
import sk.boinc.nativeboinc.news.NewsUtil;
import sk.boinc.nativeboinc.util.PreferenceName;
import sk.boinc.nativeboinc.util.ProgressState;
import sk.boinc.nativeboinc.util.ScreenOrientationHandler;
import android.app.ListActivity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Resources;
import android.os.Bundle;
import android.preference.PreferenceManager;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.BaseAdapter;
import android.widget.TextView;

/**
 * @author mat
 *
 */
public class NewsActivity extends ListActivity implements NewsFetcherListener {

	private static final String TAG = "NewsActivity";
	
	private BoincManagerApplication mApp = null;
	private SharedPreferences mSharedPrefs = null;
	
	private ScreenOrientationHandler mScreenOrientation = null;
	
	private ArrayList<NewsMessage> mNewsMessages = null;
	private long mPreviousNewsTime = -1;
	
	/* listener used outside activity (when rotated) and saved state */
	private static class SavedState implements NewsFetcherListener {

		public int mState = ProgressState.NOT_RUN;
		public boolean mIsNewFetched = false;
		private ArrayList<NewsMessage> mNewsMessages;
		public boolean mIsNewsReaded = false; // if new readed 
		public long mPreviousNewsTime; 
		
		public SavedState(NewsActivity activity) {
			mNewsMessages = activity.mNewsMessages;
			mPreviousNewsTime = activity.mPreviousNewsTime;
		}
		
		public synchronized void setInProgress() {
			mState = ProgressState.IN_PROGRESS;
		}
		
		public synchronized void setIsReaded() {
			mIsNewsReaded  = true;
		}
		
		public synchronized boolean isInProgress() {
			return mState == ProgressState.IN_PROGRESS;
		}
		
		public synchronized boolean isToUpdateNews() {
			return mState == ProgressState.FINISHED && mIsNewFetched && !mIsNewsReaded;
		}
		
		public synchronized boolean isToUpdatePreviousTime() {
			return mState == ProgressState.FINISHED && !mIsNewsReaded;
		}
		
		public void save(NewsActivity activity) {
			mNewsMessages = activity.mNewsMessages;
			mPreviousNewsTime = activity.mPreviousNewsTime;
		}
		
		public void restore(NewsActivity activity) {
			activity.mNewsMessages = mNewsMessages;
			activity.mPreviousNewsTime = mPreviousNewsTime;
		}
		
		@Override
		public synchronized void onNewsReceived(boolean isNewFetched) {
			mIsNewFetched = isNewFetched;
			mIsNewsReaded = false;
			mState = ProgressState.FINISHED;
		}

		@Override
		public synchronized void onNewsReceiveError() {
			mState = ProgressState.FAILED;
		}
	}
	
	private SavedState mSavedState = null;
	
	private class NewsMessageAdapter extends BaseAdapter {

		private Context mContext = null;
		private BoincManagerApplication mApp = null;
		private Resources mRes = null;
		
		public NewsMessageAdapter(Context context) {
			mContext = context;
			mRes = mContext.getResources();
			mApp = (BoincManagerApplication)context.getApplicationContext();
		}
		
		@Override
		public int getCount() {
			return (mNewsMessages != null) ? mNewsMessages.size() : 0; 
		}

		@Override
		public Object getItem(int id) {
			return mNewsMessages.get(id);
		}

		@Override
		public long getItemId(int position) { 
			return position;
		}

		@Override
		public View getView(int position, View convertView, ViewGroup parent) {
			View view = convertView;
			
			if (convertView == null) {
				LayoutInflater inflater = LayoutInflater.from(mContext);
				view = inflater.inflate(R.layout.news_item, null);
			}
			
			NewsMessage message = mNewsMessages.get(position);
			
			TextView titleView = (TextView)view.findViewById(R.id.title);
			TextView timeView = (TextView)view.findViewById(R.id.time);
			TextView contentView = (TextView)view.findViewById(R.id.content);
			
			int color;
			if (message.getTimestamp() > mPreviousNewsTime)
				color = mRes.getColor(R.color.newNewsColor);
			else
				color = mRes.getColorStateList(android.R.color.secondary_text_dark).getDefaultColor();
			
			titleView.setTextColor(color);
			timeView.setTextColor(color);
			contentView.setTextColor(color);
			
			titleView.setText(message.getTitle());
			timeView.setText(NewsUtil.formatDate(message.getTimestamp()));
			
			mApp.setHtmlText(contentView, message.getContent());
			return view;
		}
	}
	
	/* async task */
	private NewsReceiver.NewsFetcherTask mFetcherTask = null;
	private NewsReceiver.NewsFetcherBridge mNewsFetcherBridge = null;
	
	@Override
	protected void onCreate(Bundle savedInstanceState) {
		// enable progress
		requestWindowFeature(Window.FEATURE_INDETERMINATE_PROGRESS);
		
		super.onCreate(savedInstanceState);
		// shared prefs
		mSharedPrefs = PreferenceManager.getDefaultSharedPreferences(this);
		
		setContentView(R.layout.news_list);
		mScreenOrientation = new ScreenOrientationHandler(this);
		
		getListView().setFastScrollEnabled(true);
		setListAdapter(new NewsMessageAdapter(this));
		
		mApp = (BoincManagerApplication)getApplication();
		mNewsFetcherBridge = mApp.getNewsFetcherBridge();
		
		mSavedState = (SavedState)getLastNonConfigurationInstance();
		if (mSavedState != null) {
			mSavedState.restore(this);
			mFetcherTask = mApp.getNewsFetcherTask();
		} else { /* if real creation */
			if (Logging.DEBUG) Log.d(TAG, "Create and start updating");
			// get previous new time
			readNews();
			mSavedState = new SavedState(this);
			
			/* execute news fetcher */
			mFetcherTask = new NewsReceiver.NewsFetcherTask(mApp, true);
			mSavedState.setInProgress();
			mNewsFetcherBridge.addListener(mSavedState);
			mFetcherTask.execute(new Void[0]);
			
			// show progress
			setProgressBarIndeterminateVisibility(true);
		}
		
		((BaseAdapter)getListAdapter()).notifyDataSetChanged();
	}
	
	private void readNews() {
		mPreviousNewsTime = mSharedPrefs.getLong(PreferenceName.PREVIOUS_NEWS_TIME, Long.MAX_VALUE);
		mNewsMessages = NewsUtil.readNews(this);
		if (mNewsMessages == null)
			mNewsMessages = new ArrayList<NewsMessage>(1);
	}
	
	private void updatePreviousNewsTime() {
		long prevTimestamp = 0;
		if (mNewsMessages != null && mNewsMessages.size() != 0)
			prevTimestamp = mNewsMessages.get(0).getTimestamp();
		// set as no new news (when exiting from news)
		mSharedPrefs.edit().putLong(PreferenceName.PREVIOUS_NEWS_TIME,
				prevTimestamp).commit();
	}
	
	@Override
	protected void onNewIntent(Intent intent) {
		super.onNewIntent(intent);
		readNews();
		((BaseAdapter)getListAdapter()).notifyDataSetChanged();
	}
	
	@Override
	public Object onRetainNonConfigurationInstance() {
		mSavedState.save(this);
		return mSavedState;
	}
	
	@Override
	protected void onResume() {
		super.onResume();
		mScreenOrientation.setOrientation();
		if (mNewsFetcherBridge != null)
			mNewsFetcherBridge.addListener(this);
		if (mSavedState.isInProgress()) {
			if (mFetcherTask != null) {// if in progress
				if (Logging.DEBUG) Log.d(TAG, "During updating after resuming");
				// show progress
				setProgressBarIndeterminateVisibility(true);
			} else
				setProgressBarIndeterminateVisibility(false);
		} else {
			// if finished
			if (mSavedState.isToUpdateNews()) {
				if (Logging.DEBUG) Log.d(TAG, "Update after resuming");
				readNews();
				((BaseAdapter)getListAdapter()).notifyDataSetChanged();
				mSavedState.setIsReaded();
			}
			if (mSavedState.isToUpdatePreviousTime()) {
				// remove notification
				mApp.getNotificationController().removeNewsMessages();
				// set as no new news (when exiting from news)
				updatePreviousNewsTime();
			}
			
			// show progress
			setProgressBarIndeterminateVisibility(false);
			mFetcherTask = null;
		}
	}
	
	@Override
	protected void onPause() {
		super.onPause();
		if (mNewsFetcherBridge != null)
			mNewsFetcherBridge.removeListener(this);
	}
	
	@Override
	public void finish() {
		if (mFetcherTask != null) {
			if (Logging.DEBUG) Log.d(TAG, "cancel update task");
			// if activity finished
			mFetcherTask.cancel(true);
			mFetcherTask = null;
		}
		if (mNewsFetcherBridge != null) {
			mNewsFetcherBridge.removeListener(this); 
			mNewsFetcherBridge.removeListener(mSavedState);
		}
		super.finish();
	}
	
	@Override
	protected void onDestroy() {
		if (Logging.DEBUG) Log.d(TAG, "onDestroy");
		if (mNewsFetcherBridge != null)
			mNewsFetcherBridge.removeListener(this);
		mFetcherTask = null;
		mNewsFetcherBridge = null;
		mScreenOrientation = null;
		super.onDestroy();
		mSavedState = null;
	}

	@Override
	public void onNewsReceived(final boolean isNewFetched) {
		// updating
		if (isNewFetched) {
			if (Logging.DEBUG) Log.d(TAG, "News update at activity live");
			readNews();
			((BaseAdapter)getListAdapter()).notifyDataSetChanged();
			mSavedState.setIsReaded();
		} else
			if (Logging.DEBUG) Log.d(TAG, "No update news at activity live");
		// remove notification
		mApp.getNotificationController().removeNewsMessages();
		// set as no new news (when exiting from news)
		updatePreviousNewsTime();
		// hide progress
		setProgressBarIndeterminateVisibility(false);
		// reset fetcher task
		mFetcherTask = null;
	}

	@Override
	public void onNewsReceiveError() {
		// ignore errors but hide progress
		setProgressBarIndeterminateVisibility(false);
		// reset fetcher task
		mFetcherTask = null;
	}
}
