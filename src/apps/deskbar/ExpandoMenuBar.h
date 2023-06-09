/*
Open Tracker License

Terms and Conditions

Copyright (c) 1991-2000, Be Incorporated. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice applies to all licensees
and shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF TITLE, MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
BE INCORPORATED BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF, OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of Be Incorporated shall not be
used in advertising or otherwise to promote the sale, use or other dealings in
this Software without prior written authorization from Be Incorporated.

Tracker(TM), Be(R), BeOS(R), and BeIA(TM) are trademarks or registered
trademarks of Be Incorporated in the United States and other countries. Other
brand product names are registered trademarks or trademarks of their respective
holders.
All rights reserved.
*/
#ifndef EXPANDO_MENU_BAR_H
#define EXPANDO_MENU_BAR_H


// application list
// top level at window
// in expanded mode horizontal and vertical


#include <MenuBar.h>
#include <Locker.h>


const float kSepItemWidth = 5.0f;


enum drag_and_drop_selection {
	kNoSelection,
	kDeskbarMenuSelection,
	kAppMenuSelection,
	kAnyMenuSelection
};


class BBitmap;
class TBarView;
class TBarMenuTitle;
class TTeamMenuItem;


class TExpandoMenuBar : public BMenuBar {
public:
							TExpandoMenuBar(menu_layout layout,
								TBarView* barView = NULL);

	virtual	void			AllAttached();
	virtual	void			AttachedToWindow();
	virtual	void			DetachedFromWindow();

	virtual	void			Draw(BRect update);
	virtual	void			DrawBackground(BRect update);

	virtual	void			MessageReceived(BMessage* message);

	virtual	void			MouseDown(BPoint where);
	virtual	void			MouseMoved(BPoint where, uint32 code,
								const BMessage* message);
	virtual	void			MouseUp(BPoint where);

			void			BuildItems();

			BMenuItem*		ItemAtPoint(BPoint point);
			TTeamMenuItem*	TeamItemAtPoint(BPoint location,
								BMenuItem** _item = NULL);
			bool			InDeskbarMenu(BPoint) const;

			void			CheckItemSizes(int32 delta, bool reset = false);

			float			MinHorizontalItemWidth();
			float			MaxHorizontalItemWidth();

			menu_layout		MenuLayout() const;
			void			SetMenuLayout(menu_layout layout);

			void			SizeWindow(int32 delta);
			bool			CheckForSizeOverrun();

			void			StartMonitoringWindows();
			void			StopMonitoringWindows();

private:
	static	int				CompareByName(const void* first,
								const void* second);
	static	int32			monitor_team_windows(void* arg);

			void			AddTeam(BList* team, BBitmap* icon, char* name,
								char* signature);
			void			AddTeam(team_id team, const char* signature);
			void			RemoveTeam(team_id team, bool partial);

			void			_FinishedDrag(bool invoke = false);

			bool			CheckForSizeOverrunVertical();
			bool			CheckForSizeOverrunHorizontal();

			float			MaxHorizontalWidth();

			bool			Vertical() const
								{ return MenuLayout() == B_ITEMS_IN_COLUMN; };
private:
			TBarView*		fBarView;
			bool			fOverflow : 1;
			bool			fUnderflow : 1;
			bool			fFirstBuild : 1;

			TTeamMenuItem*	fPreviousDragTargetItem;
			BMenuItem*		fLastMousedOverItem;
			BMenuItem*		fLastClickedItem;
			bigtime_t		fLastClickTime;
			BList			fTeamList;

	static	bool			sDoMonitor;
	static	thread_id		sMonThread;
	static	BLocker			sMonLocker;
};


#endif // EXPANDO_MENU_BAR_H
