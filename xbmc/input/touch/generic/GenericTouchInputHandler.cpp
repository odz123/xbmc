/*
 *  Copyright (C) 2012-2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GenericTouchInputHandler.h"

#include "input/touch/generic/GenericTouchPinchDetector.h"
#include "input/touch/generic/GenericTouchRotateDetector.h"
#include "input/touch/generic/GenericTouchSwipeDetector.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <mutex>

using namespace std::chrono_literals;

namespace
{
constexpr auto TOUCH_HOLD_TIMEOUT = 500ms;
}

CGenericTouchInputHandler::CGenericTouchInputHandler() : m_holdTimer(new CTimer(this))
{
}

CGenericTouchInputHandler::~CGenericTouchInputHandler() = default;

CGenericTouchInputHandler& CGenericTouchInputHandler::GetInstance()
{
  static CGenericTouchInputHandler sTouchInput;
  return sTouchInput;
}

float CGenericTouchInputHandler::AdjustPointerSize(float size) const {
  if (size > 0.0f)
    return size;
  else
    // Set a default size if touch input layer does not have anything useful,
    // approx. 3.2mm
    return m_dpi / 8.0f;
}

bool CGenericTouchInputHandler::HandleTouchInput(TouchInput event,
                                                 float x,
                                                 float y,
                                                 int64_t time,
                                                 int32_t pointer /* = 0 */,
                                                 float size /* = 0.0f */)
{
  if (time < 0 || pointer < 0 || pointer >= MAX_POINTERS)
    return false;

  // Struct to capture deferred callback actions to be executed outside the lock
  struct DeferredCallbacks
  {
    enum class Type
    {
      None,
      Abort,
      SingleTouchStart,
      MultiTouchDown,
      SingleTouchUp,
      SingleTouchUpWithTap,
      PanEnd,
      MultiTouchUp,
      MultiTouchDoneEnd,
      MultiTouchDoneEndWithTap,
      MoveGestureStart,
      SingleTouchMove,
      PanMove,
      MultiTouchMove
    };
    Type type = Type::None;
    float x = 0, y = 0;
    float x2 = 0, y2 = 0; // for tap with two pointers
    float offsetX = 0, offsetY = 0;
    float velocityX = 0, velocityY = 0;
    int32_t pointer = 0;
    bool stopTimerWait = false;
    bool startTimer = false;
    bool stopTimer = false;
    bool triggerDetectorsFlag = false;
    TouchInput detectorEvent = TouchInputAbort;
    int32_t detectorPointer = 0;
    Pointer detectorPointerData;
    std::set<std::unique_ptr<IGenericTouchGestureDetector>>* detectors = nullptr;
  };

  DeferredCallbacks deferred;
  bool result = true;
  bool earlyReturn = false;
  bool returnFalse = false;

  {
    std::lock_guard lock(m_critical);

    m_pointers[pointer].current.x = x;
    m_pointers[pointer].current.y = y;
    m_pointers[pointer].current.time = time;

    switch (event)
    {
      case TouchInputAbort:
      {
        // Prepare detector triggering data
        deferred.triggerDetectorsFlag = true;
        deferred.detectorEvent = event;
        deferred.detectorPointer = pointer;
        deferred.detectorPointerData = m_pointers[pointer];
        deferred.detectors = &m_detectors;

        setGestureState(TouchGestureUnknown);
        for (auto& p : m_pointers)
          p.reset();

        deferred.type = DeferredCallbacks::Type::Abort;
        break;
      }

      case TouchInputDown:
      {
        m_pointers[pointer].down.x = x;
        m_pointers[pointer].down.y = y;
        m_pointers[pointer].down.time = time;
        m_pointers[pointer].moving = false;
        m_pointers[pointer].size = AdjustPointerSize(size);

        // If this is the down event of the primary pointer
        // we start by assuming that it's a single touch
        if (pointer == 0)
        {
          // create new gesture detectors
          m_detectors.emplace(new CGenericTouchSwipeDetector(this, m_dpi));
          m_detectors.emplace(new CGenericTouchPinchDetector(this, m_dpi));
          m_detectors.emplace(new CGenericTouchRotateDetector(this, m_dpi));

          deferred.triggerDetectorsFlag = true;
          deferred.detectorEvent = event;
          deferred.detectorPointer = pointer;
          deferred.detectorPointerData = m_pointers[pointer];
          deferred.detectors = &m_detectors;

          setGestureState(TouchGestureSingleTouch);
          deferred.type = DeferredCallbacks::Type::SingleTouchStart;
          deferred.x = x;
          deferred.y = y;
          deferred.startTimer = true;
        }
        // Otherwise it's the down event of another pointer
        else
        {
          deferred.triggerDetectorsFlag = true;
          deferred.detectorEvent = event;
          deferred.detectorPointer = pointer;
          deferred.detectorPointerData = m_pointers[pointer];
          deferred.detectors = &m_detectors;

          // If we so far assumed single touch or still have the primary
          // pointer of a previous multi touch pressed down, we can update to multi touch
          if (m_gestureState == TouchGestureSingleTouch ||
              m_gestureState == TouchGestureSingleTouchHold ||
              m_gestureState == TouchGestureMultiTouchDone)
          {
            deferred.type = DeferredCallbacks::Type::MultiTouchDown;
            deferred.x = x;
            deferred.y = y;
            deferred.pointer = pointer;
            deferred.stopTimerWait = true;

            if (m_gestureState == TouchGestureSingleTouch ||
                m_gestureState == TouchGestureSingleTouchHold)
              deferred.startTimer = true;

            setGestureState(TouchGestureMultiTouchStart);
          }
          // Otherwise we should ignore this pointer
          else
          {
            m_pointers[pointer].reset();
            returnFalse = true;
            break;
          }
        }
        earlyReturn = true;
        break;
      }

      case TouchInputUp:
      {
        // unexpected event => abort
        if (!m_pointers[pointer].valid() || m_gestureState == TouchGestureUnknown)
        {
          returnFalse = true;
          break;
        }

        deferred.triggerDetectorsFlag = true;
        deferred.detectorEvent = event;
        deferred.detectorPointer = pointer;
        deferred.detectorPointerData = m_pointers[pointer];
        deferred.detectors = &m_detectors;

        deferred.stopTimer = true;

        // Just a single tap with a pointer
        if (m_gestureState == TouchGestureSingleTouch ||
            m_gestureState == TouchGestureSingleTouchHold)
        {
          if (m_gestureState == TouchGestureSingleTouch)
          {
            deferred.type = DeferredCallbacks::Type::SingleTouchUpWithTap;
          }
          else
          {
            deferred.type = DeferredCallbacks::Type::SingleTouchUp;
          }
          deferred.x = x;
          deferred.y = y;
        }
        // A pan gesture started with a single pointer (ignoring any other pointers)
        else if (m_gestureState == TouchGesturePan)
        {
          float velocityX = 0.0f;
          float velocityY = 0.0f;
          m_pointers[pointer].velocity(velocityX, velocityY, false);

          deferred.type = DeferredCallbacks::Type::PanEnd;
          deferred.x = x;
          deferred.y = y;
          deferred.offsetX = x - m_pointers[pointer].down.x;
          deferred.offsetY = y - m_pointers[pointer].down.y;
          deferred.velocityX = velocityX;
          deferred.velocityY = velocityY;
        }
        // we are in multi-touch
        else
        {
          deferred.type = DeferredCallbacks::Type::MultiTouchUp;
          deferred.x = x;
          deferred.y = y;
          deferred.pointer = pointer;
        }

        // If we were in multi touch mode and lifted one pointer
        // we can go into the TouchGestureMultiTouchDone state which will allow
        // the user to go back into multi touch mode without lifting the primary pointer
        if (m_gestureState == TouchGestureMultiTouchStart ||
            m_gestureState == TouchGestureMultiTouchHold || m_gestureState == TouchGestureMultiTouch)
        {
          setGestureState(TouchGestureMultiTouchDone);

          // after lifting the primary pointer, the secondary pointer will
          // become the primary pointer in the next event
          if (pointer == 0)
          {
            m_pointers[0] = m_pointers[1];
            pointer = 1;
          }
        }
        // Otherwise abort
        else
        {
          if (m_gestureState == TouchGestureMultiTouchDone)
          {
            float velocityX = 0.0f;
            float velocityY = 0.0f;
            m_pointers[pointer].velocity(velocityX, velocityY, false);

            // if neither of the two pointers moved we have a single tap with multiple pointers
            if (m_gestureStateOld != TouchGestureMultiTouchHold &&
                m_gestureStateOld != TouchGestureMultiTouch)
            {
              deferred.type = DeferredCallbacks::Type::MultiTouchDoneEndWithTap;
              deferred.x2 = std::abs((m_pointers[0].down.x + m_pointers[1].down.x) / 2);
              deferred.y2 = std::abs((m_pointers[0].down.y + m_pointers[1].down.y) / 2);
            }
            else
            {
              deferred.type = DeferredCallbacks::Type::MultiTouchDoneEnd;
            }
            deferred.x = x;
            deferred.y = y;
            deferred.offsetX = x - m_pointers[pointer].down.x;
            deferred.offsetY = y - m_pointers[pointer].down.y;
            deferred.velocityX = velocityX;
            deferred.velocityY = velocityY;
          }

          setGestureState(TouchGestureUnknown);
        }
        m_pointers[pointer].reset();

        earlyReturn = true;
        break;
      }

      case TouchInputMove:
      {
        // unexpected event => abort
        if (!m_pointers[pointer].valid() || m_gestureState == TouchGestureUnknown ||
            m_gestureState == TouchGestureMultiTouchDone)
        {
          returnFalse = true;
          break;
        }

        bool moving = std::any_of(m_pointers.cbegin(), m_pointers.cend(),
                                  [](Pointer const& p) { return p.valid() && p.moving; });

        if (moving)
        {
          deferred.stopTimer = true;

          // the touch is moving so we start a gesture
          if (m_gestureState == TouchGestureSingleTouch ||
              m_gestureState == TouchGestureMultiTouchStart)
          {
            deferred.type = DeferredCallbacks::Type::MoveGestureStart;
            deferred.x = m_pointers[pointer].down.x;
            deferred.y = m_pointers[pointer].down.y;
          }
        }

        deferred.triggerDetectorsFlag = true;
        deferred.detectorEvent = event;
        deferred.detectorPointer = pointer;
        deferred.detectorPointerData = m_pointers[pointer];
        deferred.detectors = &m_detectors;

        // Check if the touch has moved far enough to count as movement
        if ((m_gestureState == TouchGestureSingleTouch ||
             m_gestureState == TouchGestureMultiTouchStart) &&
            !m_pointers[pointer].moving)
        {
          returnFalse = true;
          break;
        }

        if (m_gestureState == TouchGestureSingleTouch)
        {
          m_pointers[pointer].last.copy(m_pointers[pointer].down);
          setGestureState(TouchGesturePan);
        }
        else if (m_gestureState == TouchGestureMultiTouchStart)
        {
          setGestureState(TouchGestureMultiTouch);

          // set the starting point
          saveLastTouch();
        }

        float offsetX = x - m_pointers[pointer].last.x;
        float offsetY = y - m_pointers[pointer].last.y;
        float velocityX = 0.0f;
        float velocityY = 0.0f;
        m_pointers[pointer].velocity(velocityX, velocityY);

        if (m_pointers[pointer].moving &&
            (m_gestureState == TouchGestureSingleTouch ||
             m_gestureState == TouchGestureSingleTouchHold || m_gestureState == TouchGesturePan))
        {
          if (deferred.type == DeferredCallbacks::Type::None ||
              deferred.type == DeferredCallbacks::Type::MoveGestureStart)
          {
            // Combine with gesture start if present
            if (deferred.type == DeferredCallbacks::Type::MoveGestureStart)
            {
              // Will be handled in callback execution
            }
            deferred.type = DeferredCallbacks::Type::SingleTouchMove;
            deferred.x = x;
            deferred.y = y;
            deferred.offsetX = offsetX;
            deferred.offsetY = offsetY;
            deferred.velocityX = velocityX;
            deferred.velocityY = velocityY;
          }
        }

        // Let's see if we have a pan gesture (i.e. the primary and only pointer moving)
        if (m_gestureState == TouchGesturePan)
        {
          deferred.type = DeferredCallbacks::Type::PanMove;
          deferred.x = x;
          deferred.y = y;
          deferred.offsetX = offsetX;
          deferred.offsetY = offsetY;
          deferred.velocityX = velocityX;
          deferred.velocityY = velocityY;

          m_pointers[pointer].last.x = x;
          m_pointers[pointer].last.y = y;
        }
        else if (m_gestureState == TouchGestureMultiTouch)
        {
          if (moving)
          {
            deferred.type = DeferredCallbacks::Type::MultiTouchMove;
            deferred.x = x;
            deferred.y = y;
            deferred.offsetX = offsetX;
            deferred.offsetY = offsetY;
            deferred.velocityX = velocityX;
            deferred.velocityY = velocityY;
            deferred.pointer = pointer;
          }
        }
        else
        {
          returnFalse = true;
          break;
        }

        earlyReturn = true;
        break;
      }

      default:
        CLog::Log(LOGDEBUG, "CGenericTouchInputHandler: unknown TouchInput");
        returnFalse = true;
        break;
    }
  } // lock released here

  // Execute deferred callbacks outside of lock to prevent deadlocks
  // Handle timer operations first
  if (deferred.stopTimerWait)
    m_holdTimer->Stop(true);
  if (deferred.stopTimer)
    m_holdTimer->Stop(false);
  if (deferred.startTimer)
    m_holdTimer->Start(TOUCH_HOLD_TIMEOUT);

  // Trigger detectors - these are internal and don't call external callbacks
  if (deferred.triggerDetectorsFlag)
  {
    triggerDetectors(deferred.detectorEvent, deferred.detectorPointer);
  }

  // Execute the main callback outside of lock
  switch (deferred.type)
  {
    case DeferredCallbacks::Type::Abort:
      OnTouchAbort();
      break;

    case DeferredCallbacks::Type::SingleTouchStart:
      result = OnSingleTouchStart(deferred.x, deferred.y);
      break;

    case DeferredCallbacks::Type::MultiTouchDown:
      result = OnMultiTouchDown(deferred.x, deferred.y, deferred.pointer);
      break;

    case DeferredCallbacks::Type::SingleTouchUp:
      result = OnSingleTouchEnd(deferred.x, deferred.y);
      break;

    case DeferredCallbacks::Type::SingleTouchUpWithTap:
      result = OnSingleTouchEnd(deferred.x, deferred.y);
      OnTap(deferred.x, deferred.y, 1);
      break;

    case DeferredCallbacks::Type::PanEnd:
      result = OnTouchGestureEnd(deferred.x, deferred.y, deferred.offsetX, deferred.offsetY,
                                 deferred.velocityX, deferred.velocityY);
      break;

    case DeferredCallbacks::Type::MultiTouchUp:
      result = OnMultiTouchUp(deferred.x, deferred.y, deferred.pointer);
      break;

    case DeferredCallbacks::Type::MultiTouchDoneEnd:
      result = OnTouchGestureEnd(deferred.x, deferred.y, deferred.offsetX, deferred.offsetY,
                                 deferred.velocityX, deferred.velocityY);
      break;

    case DeferredCallbacks::Type::MultiTouchDoneEndWithTap:
      result = OnTouchGestureEnd(deferred.x, deferred.y, deferred.offsetX, deferred.offsetY,
                                 deferred.velocityX, deferred.velocityY);
      OnTap(deferred.x2, deferred.y2, 2);
      break;

    case DeferredCallbacks::Type::MoveGestureStart:
      result = OnTouchGestureStart(deferred.x, deferred.y);
      break;

    case DeferredCallbacks::Type::SingleTouchMove:
      result = OnSingleTouchMove(deferred.x, deferred.y, deferred.offsetX, deferred.offsetY,
                                 deferred.velocityX, deferred.velocityY);
      break;

    case DeferredCallbacks::Type::PanMove:
      result = OnSingleTouchMove(deferred.x, deferred.y, deferred.offsetX, deferred.offsetY,
                                 deferred.velocityX, deferred.velocityY);
      result = OnTouchGesturePan(deferred.x, deferred.y, deferred.offsetX, deferred.offsetY,
                                 deferred.velocityX, deferred.velocityY);
      break;

    case DeferredCallbacks::Type::MultiTouchMove:
      result = OnMultiTouchMove(deferred.x, deferred.y, deferred.offsetX, deferred.offsetY,
                                deferred.velocityX, deferred.velocityY, deferred.pointer);
      break;

    case DeferredCallbacks::Type::None:
    default:
      break;
  }

  if (returnFalse)
    return false;

  if (earlyReturn)
    return result;

  return false;
}

bool CGenericTouchInputHandler::UpdateTouchPointer(
    int32_t pointer, float x, float y, int64_t time, float size /* = 0.0f */)
{
  if (pointer < 0 || pointer >= MAX_POINTERS)
    return false;

  std::lock_guard lock(m_critical);

  m_pointers[pointer].last.copy(m_pointers[pointer].current);

  m_pointers[pointer].current.x = x;
  m_pointers[pointer].current.y = y;
  m_pointers[pointer].current.time = time;
  m_pointers[pointer].size = AdjustPointerSize(size);

  // calculate whether the pointer has moved at all
  if (!m_pointers[pointer].moving)
  {
    CVector down = m_pointers[pointer].down;
    CVector current = m_pointers[pointer].current;
    CVector distance = down - current;

    if (distance.length() > m_pointers[pointer].size)
      m_pointers[pointer].moving = true;
  }

  for (auto const& detector : m_detectors)
    detector->OnTouchUpdate(pointer, m_pointers[pointer]);

  return true;
}

void CGenericTouchInputHandler::saveLastTouch()
{
  for (auto& pointer : m_pointers)
    pointer.last.copy(pointer.current);
}

void CGenericTouchInputHandler::OnTimeout()
{
  // Collect callback info under lock, call callbacks outside to prevent deadlocks
  enum class TimeoutAction { None, SingleTouchHold, MultiTouchHold };
  TimeoutAction action = TimeoutAction::None;
  float x1 = 0, y1 = 0, x2 = 0, y2 = 0;

  {
    std::lock_guard lock(m_critical);

    switch (m_gestureState)
    {
      case TouchGestureSingleTouch:
        setGestureState(TouchGestureSingleTouchHold);
        action = TimeoutAction::SingleTouchHold;
        x1 = m_pointers[0].down.x;
        y1 = m_pointers[0].down.y;
        break;

      case TouchGestureMultiTouchStart:
        if (!m_pointers[0].moving && !m_pointers[1].moving)
        {
          setGestureState(TouchGestureMultiTouchHold);
          action = TimeoutAction::MultiTouchHold;
          x1 = m_pointers[0].down.x;
          y1 = m_pointers[0].down.y;
          x2 = m_pointers[1].down.x;
          y2 = m_pointers[1].down.y;
        }
        break;

      default:
        break;
    }
  }

  // Call callbacks outside of lock to prevent deadlocks
  switch (action)
  {
    case TimeoutAction::SingleTouchHold:
      OnSingleTouchHold(x1, y1);
      OnLongPress(x1, y1, 1);
      break;

    case TimeoutAction::MultiTouchHold:
      OnMultiTouchHold(x1, y1);
      OnLongPress(std::abs((x1 + x2) / 2), std::abs((y1 + y2) / 2), 2);
      break;

    default:
      break;
  }
}

void CGenericTouchInputHandler::triggerDetectors(TouchInput event, int32_t pointer)
{
  switch (event)
  {
    case TouchInputAbort:
    {
      m_detectors.clear();
      break;
    }

    case TouchInputDown:
    {
      for (auto const& detector : m_detectors)
        detector->OnTouchDown(pointer, m_pointers[pointer]);
      break;
    }

    case TouchInputUp:
    {
      for (auto const& detector : m_detectors)
        detector->OnTouchUp(pointer, m_pointers[pointer]);
      break;
    }

    case TouchInputMove:
    {
      for (auto const& detector : m_detectors)
        detector->OnTouchMove(pointer, m_pointers[pointer]);
      break;
    }

    default:
      return;
  }

  for (auto it = m_detectors.begin(); it != m_detectors.end();)
  {
    if ((*it)->IsDone())
      it = m_detectors.erase(it);
    else
      it++;
  }
}
