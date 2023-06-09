/*
 * Copyright 2006, Haiku. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Stephan Aßmus <superstippi@gmx.de>
 */

#ifndef BITMAP_EXPORTER_H
#define BITMAP_EXPORTER_H

#include "Exporter.h"

/*! Exports to an arbitrary image format.
	Uses BBitmap and a Translator to turn an image into the desired format.

	\note Currently only exports to the PNG format.
*/
class BitmapExporter : public Exporter {
 public:
								BitmapExporter(uint32 size);
	virtual						~BitmapExporter();

	virtual	status_t			Export(const Icon* icon,
									   BPositionIO* stream);

	virtual	const char*			MIMEType();

 private:
			uint32				fFormat;
			uint32				fSize;
};

#endif // BITMAP_EXPORTER_H
