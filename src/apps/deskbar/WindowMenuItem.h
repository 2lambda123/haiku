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
#ifndef WINDOW_MENU_ITEM_H
#define WINDOW_MENU_ITEM_H


#include "TruncatableMenuItem.h"

#include <String.h>


class BBitmap;

// Individual windows of an application item for WindowMenu,
// sub of TeamMenuItem all DB positions
class TWindowMenuItem : public TTruncatableMenuItem {
public:
								TWindowMenuItem(const char* name, int32 id,
									bool mini, bool currentWorkSpace,
									bool dragging = false);

			void				SetTo(const char* name, int32 id, bool minimized,
									bool local, bool dragging = false);

			bool				Expanded() const { return fExpanded; };
			void				SetExpanded(bool expand) { fExpanded = expand; };

			int32				ID() const { return fID; };
			bool				Modified() const { return fIsModified; };

			bool				RequiresUpdate() { return fRequireUpdate; };
			void				SetRequireUpdate(bool update)
									{ fRequireUpdate = update; };

	static	int32				InsertIndexFor(BMenu* menu, int32 startIndex,
									TWindowMenuItem* item);

protected:
	virtual void				GetContentSize(float* width, float* height);
	virtual void				DrawContent();
	virtual status_t			Invoke(BMessage* message = NULL);
	virtual void				Draw();

private:
			void				_Init(const char* name);

private:
			const BBitmap*		fBitmap;

			int32				fID;

			float				fLabelWidth;
			float				fLabelAscent;
			float				fLabelDescent;

			bool				fIsModified : 1;
			bool				fIsMinimized : 1;
			bool				fIsLocal : 1;
			bool				fDragging : 1;
			bool				fExpanded : 1;
			bool				fRequireUpdate : 1;
};


#endif	// WINDOW_MENU_ITEM_H
