/*
 * Copyright (C) 2006 The Android Open Source Project
 * Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.internal.telephony;

import static android.Manifest.permission.READ_PHONE_STATE;
import android.app.ActivityManagerNative;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.res.Resources;
import android.os.AsyncResult;
import android.os.Handler;
import android.os.Message;
import android.os.Power;
import android.os.PowerManager;
import android.os.Registrant;
import android.os.RegistrantList;
import android.util.Log;
import android.view.WindowManager;

import com.android.internal.telephony.PhoneBase;
import com.android.internal.telephony.CommandsInterface.RadioState;
import com.android.internal.telephony.IccCard.State;
import com.android.internal.telephony.IccCardApplicationStatus;
import com.android.internal.telephony.IccCardApplicationStatus.AppType;
import com.android.internal.telephony.IccCardStatus.CardState;
import com.android.internal.telephony.IccCardStatus.PinState;
import com.android.internal.telephony.gsm.GSMPhone;
import com.android.internal.telephony.gsm.SIMFileHandler;
import com.android.internal.telephony.gsm.SIMRecords;
import com.android.internal.telephony.UiccManager.AppFamily;
import com.android.internal.telephony.cat.CatService;
import com.android.internal.telephony.cdma.CDMALTEPhone;
import com.android.internal.telephony.cdma.CDMAPhone;
import com.android.internal.telephony.cdma.CdmaSubscriptionSourceManager;
import com.android.internal.telephony.cdma.RuimFileHandler;
import com.android.internal.telephony.cdma.RuimRecords;

import android.os.SystemProperties;

import com.android.internal.R;

/**
 * {@hide}
 */
public class UiccCard {
    protected static final String LOG_TAG = "RIL_UiccCard";
    protected static final boolean DBG = true;

    private CardState mCardState;
    private PinState mUniversalPinState;
    private int mGsmUmtsSubscriptionAppIndex;
    private int mCdmaSubscriptionAppIndex;
    private int mImsSubscriptionAppIndex;
    private UiccCardApplication[] mUiccApplications = new UiccCardApplication[IccCardStatus.CARD_MAX_APPS];
    private Context mContext;
    private CommandsInterface mCi;
    private CatService mCatService;
    private boolean mDestroyed = false; //set to true once this card is commanded to be disposed of.
    
    private RegistrantList mAbsentRegistrants = new RegistrantList();

    private static final int EVENT_CARD_REMOVED = 13;
    private static final int EVENT_CARD_ADDED = 14;

    public UiccCard(Context c, CommandsInterface ci, IccCardStatus ics) {
        log("Creating");
        update(c, ci, ics);
    }

    public void dispose() {
        log("Disposing card");
        if (mCatService != null) mCatService.dispose();
        for (UiccCardApplication app : mUiccApplications) {
            if (app != null) {
                app.dispose();
            }
        }
        mCatService = null;
        mUiccApplications = null;
    }

    public void update(Context c, CommandsInterface ci, IccCardStatus ics) {
        if (mDestroyed) {
            loge("Updated after destroyed! Fix me!");
            return;
        }
        CardState oldState = mCardState;
        mCardState = ics.mCardState;
        mUniversalPinState = ics.mUniversalPinState;
        mGsmUmtsSubscriptionAppIndex = ics.mGsmUmtsSubscriptionAppIndex;
        mCdmaSubscriptionAppIndex = ics.mCdmaSubscriptionAppIndex;
        mImsSubscriptionAppIndex = ics.mImsSubscriptionAppIndex;
        mContext = c;
        mCi = ci;
        //update applications
        log(ics.mApplications.length + " applications");
        for ( int i = 0; i < mUiccApplications.length; i++) {
            if (mUiccApplications[i] == null) {
                //Create newly added Applications
                if (i < ics.mApplications.length) {
                    mUiccApplications[i] = new UiccCardApplication(this,
                            ics.mApplications[i], mContext, mCi);
                }
            } else if (i >= ics.mApplications.length) {
                //Delete removed applications
                mUiccApplications[i].dispose();
                mUiccApplications[i] = null;
            } else {
                //Update the rest
                mUiccApplications[i].update(ics.mApplications[i], mContext, mCi);
            }
        }

        if (mUiccApplications.length > 0 && mUiccApplications[0] != null) {
            // Initialize or Reinitialize CatService
            mCatService = CatService.getInstance(mCi,
                                                 mContext,
                                                 this);
        } else {
            if (mCatService != null) {
                mCatService.dispose();
            }
            mCatService = null;
        }

        if (oldState != CardState.CARDSTATE_ABSENT && mCardState == CardState.CARDSTATE_ABSENT) {
            mAbsentRegistrants.notifyRegistrants();
            mHandler.sendMessage(mHandler.obtainMessage(EVENT_CARD_REMOVED, null));
        } else if (oldState == CardState.CARDSTATE_ABSENT && mCardState != CardState.CARDSTATE_ABSENT) {
            mHandler.sendMessage(mHandler.obtainMessage(EVENT_CARD_ADDED, null));
        }
    }

    protected void finalize() {
        log("UiccCard finalized");
    }

    /**
     * Notifies handler of any transition into State.ABSENT
     */
    public void registerForAbsent(Handler h, int what, Object obj) {
        Registrant r = new Registrant (h, what, obj);

        mAbsentRegistrants.add(r);

        if (mCardState == CardState.CARDSTATE_ABSENT) {
            r.notifyRegistrant();
        }
    }

    public void unregisterForAbsent(Handler h) {
        mAbsentRegistrants.remove(h);
    }

    private void onIccSwap(boolean isAdded) {
        // TODO: Here we assume the device can't handle SIM hot-swap
        //      and has to reboot. We may want to add a property,
        //      e.g. REBOOT_ON_SIM_SWAP, to indicate if modem support
        //      hot-swap.
        DialogInterface.OnClickListener listener = null;


        // TODO: SimRecords is not reset while SIM ABSENT (only reset while
        //       Radio_off_or_not_available). Have to reset in both both
        //       added or removed situation.
        listener = new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                if (which == DialogInterface.BUTTON_POSITIVE) {
                    log("Reboot due to SIM swap");
                    PowerManager pm = (PowerManager) mContext
                            .getSystemService(Context.POWER_SERVICE);
                    pm.reboot("SIM is added.");
                }
            }

        };

        Resources r = Resources.getSystem();

        String title = (isAdded) ? r.getString(R.string.sim_added_title) :
            r.getString(R.string.sim_removed_title);
        String message = (isAdded) ? r.getString(R.string.sim_added_message) :
            r.getString(R.string.sim_removed_message);
        String buttonTxt = r.getString(R.string.sim_restart_button);

        AlertDialog dialog = new AlertDialog.Builder(mContext)
            .setTitle(title)
            .setMessage(message)
            .setPositiveButton(buttonTxt, listener)
            .create();
        dialog.getWindow().setType(WindowManager.LayoutParams.TYPE_SYSTEM_ALERT);
        dialog.show();
    }

    protected Handler mHandler = new Handler() {
        @Override
        public void handleMessage(Message msg){
            if (mDestroyed) {
                Log.e(LOG_TAG, "Received message " + msg + "[" + msg.what
                        + "] while being destroyed. Ignoring.");
                return;
            }

            switch (msg.what) {
                case EVENT_CARD_REMOVED:
                    onIccSwap(false);
                    break;
                case EVENT_CARD_ADDED:
                    onIccSwap(true);
                    break;
                default:
                    loge("Unknown Event " + msg.what);
            }
        }
    };

    public boolean isApplicationOnIcc(IccCardApplicationStatus.AppType type) {
        for (int i = 0 ; i < mUiccApplications.length; i++) {
            if (mUiccApplications[i] != null && mUiccApplications[i].getType() == type) {
                return true;
            }
        }
        return false;
    }

    public CardState getCardState() {
        return mCardState;
    }
 
    public PinState getUniversalPinState() {
        return mUniversalPinState;
    }

    public UiccCardApplication getApplication(AppFamily family) {
        int index = IccCardStatus.CARD_MAX_APPS;
        switch (family) {
            case APP_FAM_3GPP:
                index = mGsmUmtsSubscriptionAppIndex;
                break;
            case APP_FAM_3GPP2:
                index = mCdmaSubscriptionAppIndex;
                break;
            case APP_FAM_IMS:
                index = mImsSubscriptionAppIndex;
                break;
        }
        if (index >= 0 && index < mUiccApplications.length) {
            return mUiccApplications[index];
        }
        return null;
    }

    public UiccCardApplication getApplication(int index) {
        if (index >= 0 && index < mUiccApplications.length) {
            return mUiccApplications[index];
        }
        return null;
    }

    private void log(String msg) {
        if (DBG) Log.d(LOG_TAG, msg);
    }

    private void loge(String msg) {
        Log.e(LOG_TAG, msg);
    }
}