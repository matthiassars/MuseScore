/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2023 MuseScore Limited
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "masklayout.h"

#include "dom/barline.h"
#include "dom/chord.h"
#include "dom/jump.h"
#include "dom/lyrics.h"
#include "dom/marker.h"
#include "dom/measure.h"
#include "dom/measurenumber.h"
#include "dom/note.h"
#include "dom/parenthesis.h"
#include "dom/segment.h"
#include "dom/staff.h"
#include "dom/stafflines.h"
#include "dom/system.h"
#include "dom/page.h"
#include "dom/textlinebase.h"
#include "dom/tie.h"
#include "dom/timesig.h"
#include "dom/keysig.h"

using namespace mu::engraving;
using namespace mu::engraving::rendering::score;

void MaskLayout::computeMasks(LayoutContext& ctx, Page* page)
{
    TRACEFUNC;

    bool maskBarlines = ctx.conf().styleB(Sid::maskBarlinesForText);

    for (const System* system : page->systems()) {
        std::vector<TextBase*> allSystemText = collectAllSystemText(system);

        for (MeasureBase* mb : system->measures()) {
            if (!mb->isMeasure()) {
                continue;
            }
            Measure* measure = toMeasure(mb);

            if (maskBarlines) {
                for (const Segment& seg : measure->segments()) {
                    if (seg.isType(SegmentType::BarLineType)) {
                        computeBarlineMasks(&seg, system, allSystemText, ctx);
                    }
                }
            }

            staff_idx_t nstaves = ctx.dom().nstaves();
            for (staff_idx_t staffIdx = 0; staffIdx < nstaves; ++staffIdx) {
                if (staffIdx >= system->staves().size() || !system->staff(staffIdx)->show()) {
                    continue;
                }
                const Staff* staff = ctx.dom().staff(staffIdx);
                const StaffType* staffType = staff->staffType(measure->tick());
                StaffLines* staffLines = measure->staffLines(staffIdx);
                if (staffType->isTabStaff()) {
                    maskTABStringLinesForFrets(staffLines, ctx);
                }
            }
        }

        computeTieMasks(system, ctx);
    }
}

void MaskLayout::computeBarlineMasks(const Segment* barlineSement, const System* system, const std::vector<TextBase*>& allSystemText,
                                     LayoutContext& ctx)
{
    if (barlineSement->measure()->isLastInSystem() && barlineSement == barlineSement->measure()->lastEnabled()) {
        return;
    }

    staff_idx_t nstaves = ctx.dom().nstaves();

    std::vector<BarLine*> barlines;
    barlines.reserve(nstaves);

    for (staff_idx_t staffIdx = 0; staffIdx < ctx.dom().nstaves(); ++staffIdx) {
        if (!system->staff(staffIdx)->show()) {
            continue;
        }
        BarLine* barline = toBarLine(barlineSement->element(staff2track(staffIdx)));
        if (!barline || !barline->spanStaff()) {
            continue;
        }
        maskBarlineForText(barline, allSystemText);
    }
}

void MaskLayout::maskBarlineForText(BarLine* barline, const std::vector<TextBase*>& allSystemText)
{
    TRACEFUNC;

    PointF barlinePos = barline->pagePos();
    Shape barlineShape = barline->shape().translated(barlinePos);

    Shape mask;
    const double spatium = barline->spatium();

    for (TextBase* text : allSystemText) {
        const double fontSizeScaleFactor = text->size() / 10.0;
        const double collisionPadding = 0.2 * spatium * fontSizeScaleFactor;
        const bool hasFrame = text->frameType() != FrameType::NO_FRAME;
        const bool useHighResShape = !text->isDynamic() && !text->hasFrame();
        const double maskPadding = hasFrame ? 0.0 : std::clamp(0.5 * spatium * fontSizeScaleFactor, 0.1 * spatium, spatium);

        PointF textPos = text->pagePos();
        if (!barlineShape.intersects(text->ldata()->bbox().translated(textPos).padded(collisionPadding))) {
            continue;
        }

        Shape textShape = (useHighResShape ? text->ldata()->highResShape() : text->ldata()->shape()).translated(textPos);

        Shape filteredTextShape;
        filteredTextShape.elements().reserve(textShape.elements().size());
        for (const ShapeElement& el : textShape.elements()) {
            if (barlineShape.intersects(el.padded(collisionPadding))) {
                filteredTextShape.add(el);
            }
        }
        if (filteredTextShape.empty()) {
            continue;
        }

        filteredTextShape = filteredTextShape.bbox();
        filteredTextShape.pad(maskPadding);

        mask.add(filteredTextShape.translate(-barlinePos));
    }

    if (mask.empty()) {
        barline->mutldata()->setMask(mask);
        return;
    }

    // Ensure that we don't leave tiny barline fragments. If two masking
    // elements are too close to each other we extend them to join.
    barlineShape.translate(-barlinePos);
    const double minFragmentLengh = 0.5 * spatium;
    cleanupMask(barlineShape, mask, minFragmentLengh);

    barline->mutldata()->setMask(mask);
}

void MaskLayout::cleanupMask(const Shape& itemShape, Shape& mask, double minFragmentLength)
{
    for (size_t i = 0; i < mask.size(); ++i) {
        ShapeElement& el = mask.elements()[i];

        if (el.top() - itemShape.top() < minFragmentLength) {
            el.adjust(0.0, -minFragmentLength, 0.0, 0.0);
        }
        if (itemShape.bottom() - el.bottom() < minFragmentLength) {
            el.adjust(0.0, 0.0, 0.0, minFragmentLength);
        }

        if (el.left() + itemShape.left() < minFragmentLength) {
            el.adjust(-minFragmentLength, 0.0, 0.0, 0.0);
        }
        if (itemShape.right() - el.right() < minFragmentLength) {
            el.adjust(0.0, 0.0, minFragmentLength, 0.0);
        }

        for (size_t j = i + 1; j < mask.size(); ++j) {
            ShapeElement& otherEl = mask.elements()[j];
            if (intersects(el.left(), el.right(), otherEl.left(), otherEl.right())) {
                bool otherIsBelow = otherEl.y() > el.y();
                double vertClearance = otherIsBelow ? otherEl.top() - el.bottom() : el.top() - otherEl.bottom();
                if (vertClearance < minFragmentLength) {
                    (otherIsBelow ? el : otherEl).adjust(0.0, 0.0, 0.0, minFragmentLength);
                }
            }
            if (intersects(el.top(), el.bottom(), otherEl.top(), el.bottom())) {
                bool otherIsRight = otherEl.x() > el.x();
                double horClearance = otherIsRight ? otherEl.left() - el.right() : el.left() - otherEl.right();
                if (horClearance < minFragmentLength) {
                    (otherIsRight ? el : otherEl).adjust(0.0, 0.0, minFragmentLength, 0.0);
                }
            }
        }
    }
}

std::vector<TextBase*> MaskLayout::collectAllSystemText(const System* system)
{
    TRACEFUNC;

    std::vector<TextBase*> allText;

    for (const MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        const Measure* measure = toMeasure(mb);
        for (staff_idx_t i = 0; i < measure->mstaves().size(); ++i) {
            if (measure->showMeasureNumberOnStaff(i)) {
                if (MeasureNumber* measureNumber = measure->measureNumber(i)) {
                    allText.push_back(measureNumber);
                }
            }
        }
        for (EngravingItem* item : measure->el()) {
            const SysStaff* staff = system ? system->staff(item->staffIdx()) : nullptr;
            const bool staffVisible = staff && staff->show();
            if ((item->isMarker() || item->isJump()) && item->visible() && staffVisible) {
                allText.push_back(toTextBase(item));
            }
        }
        for (const Segment& s : measure->segments()) {
            if (!s.isType(Segment::CHORD_REST_OR_TIME_TICK_TYPE) || !s.enabled()) {
                continue;
            }
            for (EngravingItem* annotation : s.annotations()) {
                const SysStaff* staff = system ? system->staff(annotation->staffIdx()) : nullptr;
                const bool staffVisible = staff && staff->show();
                if (annotation->isTextBase() && annotation->visible() && staffVisible) {
                    allText.push_back(toTextBase(annotation));
                }
            }
            if (!s.isChordRestType()) {
                continue;
            }
            for (EngravingItem* chordRest : s.elist()) {
                if (!chordRest || !system->staff(chordRest->staffIdx())->show()) {
                    continue;
                }
                for (Lyrics* lyr : toChordRest(chordRest)->lyrics()) {
                    if (lyr->visible()) {
                        allText.push_back(lyr);
                    }
                }
            }
        }
    }

    for (SpannerSegment* spannerSegment : system->spannerSegments()) {
        if (!spannerSegment->isTextLineBaseSegment() || !system->staff(spannerSegment->staffIdx())->show()
            || !spannerSegment->getProperty(Pid::VISIBLE).toBool()) {
            continue;
        }
        TextLineBaseSegment* textLineBaseSegment = static_cast<TextLineBaseSegment*>(spannerSegment);
        Text* beginText = textLineBaseSegment->text();
        Text* endText = textLineBaseSegment->endText();
        if (beginText && !beginText->empty()) {
            allText.push_back(beginText);
        }
        if (endText && !endText->empty()) {
            allText.push_back(endText);
        }
    }

    return allText;
}

void MaskLayout::maskTABStringLinesForFrets(StaffLines* staffLines, const LayoutContext& ctx)
{
    staff_idx_t staffIdx = staffLines->staffIdx();
    bool linesThrough = ctx.dom().staff(staffIdx)->staffType(Fraction())->linesThrough();

    PointF staffLinesPos = staffLines->pagePos();

    double padding = ctx.conf().styleAbsolute(Sid::tabFretPadding);

    track_idx_t startTrack = staff2track(staffIdx);
    track_idx_t endTrack = startTrack + VOICES;

    Shape mask;

    auto maskFret = [&mask, linesThrough, padding, staffLinesPos](Chord* chord) {
        for (Note* note : chord->notes()) {
            if (!note->visible()) {
                continue;
            }
            if (!note->shouldHideFret() && (!linesThrough || note->fretConflict())) {
                Shape noteShape = note->ldata()->bbox();
                noteShape.translate(note->pagePos());

                noteShape.pad(padding);
                mask.add(noteShape.translated(-staffLinesPos));
            }
        }
    };

    auto maskParens = [&mask, linesThrough, padding, staffLinesPos](Chord* chord) {
        if (linesThrough) {
            return;
        }
        for (auto& i : chord->noteParens()) {
            const Parenthesis* leftParen = i.leftParen;
            const Parenthesis* rightParen = i.rightParen;
            bool allHidden = false;
            for (const Note* note : i.notes) {
                if (!note->shouldHideFret()) {
                    allHidden = false;
                    break;
                }
            }

            if (allHidden) {
                continue;
            }

            // HACK: Overlapping mask regions which have the same y value and height cause errors
            // Parentheses on single fret marks have the same height as the fret marks
            static const double PAREN_PADDING_EPSILON = 1.0;
            if (leftParen && leftParen->visible()) {
                Shape leftParenShape = leftParen->ldata()->bbox().translated(leftParen->pagePos());
                leftParenShape.pad(padding);
                leftParenShape.adjust(0, -PAREN_PADDING_EPSILON, 0, PAREN_PADDING_EPSILON);
                mask.add(leftParenShape.translated(-staffLinesPos));
            }

            if (rightParen && rightParen->visible()) {
                Shape rightParenShape = rightParen->ldata()->bbox().translated(rightParen->pagePos());
                rightParenShape.pad(padding);
                rightParenShape.adjust(0, -PAREN_PADDING_EPSILON, 0, PAREN_PADDING_EPSILON);
                mask.add(rightParenShape.translated(-staffLinesPos));
            }
        }
    };

    const Measure* measure = staffLines->measure();
    for (Segment* seg = measure->first(SegmentType::ChordRest); seg; seg = seg->next(SegmentType::ChordRest)) {
        for (track_idx_t track = startTrack; track < endTrack; ++track) {
            EngravingItem* el = seg->element(track);
            if (!el || !el->isChord()) {
                continue;
            }
            maskFret(toChord(el));
            maskParens(toChord(el));
            for (Chord* grace : toChord(el)->graceNotes()) {
                maskFret(grace);
                maskParens(grace);
            }
        }
    }

    staffLines->mutldata()->setMask(mask);
}

void MaskLayout::computeTieMasks(const System* system, LayoutContext& ctx)
{
    TRACEFUNC;

    const double maskPadding = .1 * system->spatium();
    const double minFragmentLengh = .25 * system->spatium();

    // loop over all tie segments in system
    for (SpannerSegment* spannerSeg : system->spannerSegments()) {
        if (!spannerSeg || !spannerSeg->isTieSegment() || !spannerSeg->visible()) {
            continue;
        }

        TieSegment* tieSeg = toTieSegment(spannerSeg);
        PointF tiePos = tieSeg->pagePos();
        Shape tieSegShape = tieSeg->shape().translated(tiePos);
        Note* startNote = tieSeg->tie()->startNote();
        Note* endNote = tieSeg->tie()->endNote();
        if (!startNote || !endNote) {
            continue;
        }
        Chord* startCh = startNote->chord();
        Chord* endCh = endNote->chord();
        Measure* startMeasure = startCh->measure();
        Measure* endMeasure = endCh->measure();
        staff_idx_t topStaffIdx = std::min(startCh->vStaffIdx(), endCh->vStaffIdx());
        staff_idx_t bottomStaffIdx = std::max(startCh->vStaffIdx(), endCh->vStaffIdx());

        Shape mask;

        endMeasure = endMeasure->nextMeasure();
        for (Measure* m = startMeasure; m && m != endMeasure; m = m->nextMeasure()) {
            if (m->system() != system) {
                continue;
            }

            for (const Segment& seg : m->segments()) {
                // loop over staves (this could be > 1 in case of staff crossing)
                for (staff_idx_t staffIdx = topStaffIdx; staffIdx <= bottomStaffIdx; staffIdx++) {
                    if (!ctx.dom().staff(staffIdx)
                        || !ctx.dom().staff(staffIdx)->show()
                        || !m->visible(staffIdx)) {
                        continue;
                    }

                    EngravingItem* item = seg.element(staff2track(staffIdx));
                    if (!item || !item->visible()
                        || !(seg.isTimeSigType()
                             || seg.isKeySigType()
                             || seg.isClefType())) {
                        continue;
                    }

                    PointF itemPos = item->pagePos();
                    Shape itemShape = item->ldata()->shape().translated(itemPos);
                    for (const ShapeElement& el : itemShape.elements()) {
                        if (tieSegShape.intersects(el)) {
                            // add a rectangle to the mask as wide as the intersecting
                            // element, but as tall as the tie's bounding box, to avoid
                            // corner cutouts
                            mask.add(RectF(el.x(), tieSegShape.bbox().y(),
                                           el.width(), tieSegShape.bbox().height()));
                        }
                    }
                }
            }
        }

        if (mask.empty()) {
            spannerSeg->mutldata()->setMask(mask);
            continue;
        }

        mask.pad(maskPadding);
        mask.translate(-tiePos);
        tieSegShape.translate(-tiePos);
        cleanupMask(tieSegShape, mask, minFragmentLengh);
        spannerSeg->mutldata()->setMask(mask);
    }
}
