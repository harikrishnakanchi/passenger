/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2014 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_SERVER_KIT_CHANNEL_H_
#define _PASSENGER_SERVER_KIT_CHANNEL_H_

#include <oxt/backtrace.hpp>
#include <oxt/macros.hpp>
#include <boost/noncopyable.hpp>
#include <boost/move/core.hpp>
#include <cassert>
#include <ServerKit/Context.h>
#include <MemoryKit/mbuf.h>
#include <Logging.h>
#include <Utils/StrIntUtils.h>

namespace Passenger {
namespace ServerKit {

using namespace boost;


/**
 * A building block for consuming buffers partially and asynchronously. When writing
 * evented servers, handling incoming data poses many problems. You might not be immediately
 * able to handle all data that you receive over a single `read()` call. For example,
 * after parsing request headers, you might want to create a temp file for storing the
 * request body, and you can't parse the request body until the temp file is created. If
 * you received the headers and (a part of) the request body in the same `read()` call
 * then you have to buffer the partially received request body. Writing this code is
 * error-prone, its flow is hard to test (because it depends on network conditions),
 * and it's ridden with boilerplate.
 *
 * The Channel class solves this problem with a nice abstraction. First, you attach
 * a data callback to a Channel. Whatever is written to the Channel, will be forwarded to
 * the data callback.
 *
 * The data callback can consume the buffer immediately, and tell Channel how many bytes
 * it has consumed, by returning an integer. If the buffer was not fully consumed then
 * Channel will call the data callback again with the remainder of the buffer. This repeats
 * until the buffer is fully consumed, or (if proper hooks are provided) until the
 * client is disconnected.
 *
 * The data callback can also tell Channel that it wants to consume the buffer
 * *asynchronously*, by returning a negative integer. At some later point, something
 * must notify Channel that the buffer is consumed, by calling `channel.consumed()`.
 * Until that happens, the Channel will tell the writer that it is not accepting any new
 * data, so that the writer can stop writing temporarily. When the buffer is consumed,
 * the Channel notifies the writer about this so that it can continue writing.
 *
 * Typical usage goes like this:
 *
 * 1. Write to the Channel using `channel.write()`.
 * 2. Check whether `channel.acceptingInput()`. If so, continue writing. If not, and
 *    `!channel.hasError()` stop writing and install an idle callback with
 *    `channel.idleCallback = ...`.
 * 3. When the idle callback is called, set `channel.idleCallback = NULL` and resume
 *    writing to the channel.
 *
 * A good example of this is FdChannel. It reads data from a file descriptor using
 * `read()`, then writes them to a Channel. It stops reading from the file descriptor
 * when the Channel is not accepting reads, and it starts reading from the file
 * descriptor when the channel is accepting reads again.
 */
class Channel: public boost::noncopyable {
public:
	struct Result {
		int consumed;
		bool end;

		Result() { }

		Result(int _consumed, bool _end)
			: consumed(_consumed),
			  end(_end)
			{ }
	};

	typedef Result (*DataCallback)(Channel *channel, const MemoryKit::mbuf &buffer, int errcode);
	typedef   void (*Callback)(Channel *channel);

	enum State {
		/**
		 * No data is available. We're waiting for data to be fed.
		 */
		IDLE,

		/**
		 * Fed data has been passed to the callback, and we're now
		 * waiting for the callback to return.
		 */
		CALLING,

		/**
		 * The callback indicated that it will call `consumed()` later.
		 * We're now waiting for that call.
		 */
		WAITING_FOR_CALLBACK,

		/**
		 * `stop()` was called while we were in the IDLE state.
		 * No data will be passed to the callback.
		 */
		STOPPED,

		/**
		 * `stop()` was called while we were in the CALLING state.
		 * When the callback completes, we will transition to STOPPED,
		 * and no further data will be passed to the callback until
		 * `start()` is called.
		 */
		STOPPED_WHILE_CALLING,

		/**
		 * `stop()` was called while we were in the WAITING_FOR_CALLBACK state.
		 * When the callback completes, we will transition to `STOPPED`,
		 * and no further data will be passed to the callback until
		 * `start()` is called.
		 */
		STOPPED_WHILE_WAITING,

		/**
		 * `start()` was called while we were in the STOPPED state,
		 * or `consumed()` was called while we were in the WAITING_FOR_CALLBACK.
		 *
		 * On the next event loop tick, we will either transition to CALLING
		 * and call the callback, or transition to IDLE, depending on whether
		 * there is data to pass to the callback.
		 */
		PLANNING_TO_CALL,

		/**
		 * An end-of-file or error has been passed to the callback, and we're
		 * now waiting for the callback to return.
		 */
		CALLING_WITH_EOF,

		/**
		 * An end-of-file or error has been passed to the callback, but the
		 * callback hasn't called `consumed()` yet.
		 */
		EOF_WAITING,

		/**
		 * An end-of-file or error has been passed to the callback, and the
		 * callback has returned and completed.
		 */
		EOF_REACHED
	};

protected:
	State state: 4;
	/** ID of the next event loop tick callback. */
	unsigned int planId: 28;
	/** If an error occurred, the errno code is stored here. 0 means no error. */
	int errcode;
	/** Buffer that will be (or is being) passed to the callback. */
	MemoryKit::mbuf buffer;
	Context *ctx;
	unsigned int generation;

	void callDataCallback() {
		RefGuard guard(hooks, this);
		callDataCallbackWithoutRefGuard();
	}

	void callDataCallbackWithoutRefGuard() {
		unsigned int generation = this->generation;
		bool done = false;
		Result cbResult;

		do {
			assert(state == CALLING || state == CALLING_WITH_EOF);
			assert(state != CALLING || !buffer.empty());
			assert(state != CALLING_WITH_EOF || buffer.empty());

			cbResult = dataCallback(this, buffer, errcode);
			if (generation != this->generation) {
				// Callback deinitialized this object.
				return;
			}
			if (cbResult.consumed > (int) buffer.size()) {
				cbResult.consumed = (int) buffer.size();
			}

			assert(state != IDLE);
			assert(state != WAITING_FOR_CALLBACK);
			assert(state != STOPPED);
			assert(state != STOPPED_WHILE_WAITING);
			assert(state != PLANNING_TO_CALL);
			assert(state != EOF_WAITING);

			if (cbResult.consumed >= 0) {
				if ((unsigned int) cbResult.consumed == buffer.size()) {
					buffer = MemoryKit::mbuf();
				} else {
					buffer = MemoryKit::mbuf(buffer, cbResult.consumed);
				}

				switch (state) {
				case CALLING:
					if (cbResult.end) {
						state = EOF_REACHED;
						done = true;
						callEndAckCallback();
					} else if (buffer.empty()) {
						state = IDLE;
						done = true;
						callIdleCallback();
					}
					// else: continue loop and call again with remaining data
					break;
				case STOPPED_WHILE_CALLING:
					if (cbResult.end) {
						state = EOF_REACHED;
						done = true;
						callEndAckCallback();
					} else {
						state = STOPPED;
						done = true;
					}
					break;
				case CALLING_WITH_EOF:
					state = EOF_REACHED;
					done = true;
					callEndAckCallback();
					break;
				case EOF_REACHED:
					state = EOF_REACHED;
					done = true;
					break;
				default:
					P_BUG("Unknown state" << toString((int) state));
					break;
				}

			} else {
				switch (state) {
				case CALLING:
					state = WAITING_FOR_CALLBACK;
					done = true;
					break;
				case STOPPED_WHILE_CALLING:
					state = STOPPED_WHILE_WAITING;
					done = true;
					break;
				case CALLING_WITH_EOF:
				case EOF_REACHED:
					state = EOF_WAITING;
					done = true;
					break;
				default:
					P_BUG("Unknown state" << toString((int) state));
					break;
				}
			}

			if (!done && hooks != NULL && hooks->impl != NULL) {
				done = !hooks->impl->hook_isConnected(hooks, this);
			}
		} while (!done);
	}

	void planNextActivity() {
		if (buffer.empty()) {
			state = IDLE;
			callIdleCallback();
		} else {
			state = PLANNING_TO_CALL;
			planId = ctx->libev->runLater(boost::bind(
				&Channel::executeCall, this));
		}
	}

	void executeCall() {
		assert(state == PLANNING_TO_CALL);
		planId = 0;
		state = CALLING;
		callDataCallback();
	}

	void callIdleCallback() {
		if (idleCallback != NULL) {
			idleCallback(this);
		}
	}

	void callEndAckCallback() {
		if (endAckCallback != NULL) {
			endAckCallback(this);
		}
	}

public:
	DataCallback dataCallback;
	Callback idleCallback;
	Callback endAckCallback;
	Hooks *hooks;

	/**
	 * Creates a Channel without a context. It doesn't work properly yet until
	 * you call `setContext()`.
	 */
	Channel()
		: state(EOF_REACHED),
		  planId(0),
		  errcode(0),
		  ctx(NULL),
		  generation(0),
		  dataCallback(NULL),
		  idleCallback(NULL),
		  endAckCallback(NULL),
		  hooks(NULL)
		{ }

	/**
	 * Creates a Channel with the given context, which must be non-NULL.
	 */
	Channel(Context *context)
		: state(IDLE),
		  planId(0),
		  errcode(0),
		  ctx(context),
		  generation(0),
		  dataCallback(NULL),
		  idleCallback(NULL),
		  endAckCallback(NULL),
		  hooks(NULL)
		{ }

	~Channel() {
		if (ctx != NULL) {
			ctx->libev->cancelCommand(planId);
		}
	}

	/**
	 * Sets the context in case you constructed a Channel without one.
	 * The Channel object doesn't work until you've set a context.
	 * May only be called right after construction.
	 */
	void setContext(Context *context) {
		ctx = context;
	}

	/**
	 * Reinitialize the Channel to its starting state so that you can reuse the
	 * object. You may only call this after calling `deinitialize()`.
	 */
	void reinitialize() {
		state   = IDLE;
		errcode = 0;
	}

	/**
	 * Deinitialize the channel and reset it into a terminal state.
	 * Whatever operations it was doing in the background will be canceled.
	 * After deinitializing, you may reinitialize it and reuse the Channel.
	 */
	void deinitialize() {
		if (ctx != NULL) {
			ctx->libev->cancelCommand(planId);
		}
		planId = 0;
		buffer = MemoryKit::mbuf();
		generation++;
	}

	/**
	 * Feed data to the Channel. The data will be passed to the callback.
	 *
	 * @pre acceptingInput()
	 */
	void feed(const MemoryKit::mbuf &mbuf) {
		MemoryKit::mbuf mbuf_copy(mbuf);
		feed(boost::move(mbuf_copy));
	}

	void feed(BOOST_RV_REF(MemoryKit::mbuf) mbuf) {
		RefGuard guard(hooks, this);
		feedWithoutRefGuard(mbuf);
	}

	/**
	 * A special version of `feed()` which does not call `hooks->hook_ref()`
	 * and `hooks->hook_unref()`. Use it in certain optimization scenarios,
	 * where you are sure that extra reference counts are not needed.
	 *
	 * @pre acceptingInput()
	 */
	void feedWithoutRefGuard(const MemoryKit::mbuf &mbuf) {
		MemoryKit::mbuf mbuf_copy(mbuf);
		feedWithoutRefGuard(boost::move(mbuf_copy));
	}

	void feedWithoutRefGuard(BOOST_RV_REF(MemoryKit::mbuf) mbuf) {
		assert(state == IDLE);
		if (mbuf.empty()) {
			state = CALLING_WITH_EOF;
		} else {
			state = CALLING;
		}
		buffer = mbuf;
		callDataCallbackWithoutRefGuard();
	}

	/**
	 * Tell the Channel that an error has occurred.
	 *
	 * If this method is called while the data callback is not active, and the
	 * channel is idle, then the error will be passed to the callback immediately.
	 * Otherwise (if the channel isn't idle), it will be passed to the callback
	 * when the channel becomes idle.
	 *
	 * If this method is called inside the data callback, or (if the data callback returned -1)
	 * when the data callback hasn't called `consumed()` yet, then the channel transitions
	 * to the end state immediately, and stops calling the data callback even when the
	 * current invocation of the data callback doesn't fully consume the buffer.
	 *
	 * Once an error has been fed, no more data will be accepted by `feed()`.
	 */
	void feedError(int errcode) {
		assert(errcode != 0);
		switch (state) {
		case IDLE:
			this->errcode = errcode;
			state = CALLING_WITH_EOF;
			callDataCallback();
			break;
		case CALLING:
		case WAITING_FOR_CALLBACK:
		case CALLING_WITH_EOF:
		case EOF_WAITING:
			this->errcode = errcode;
			state = EOF_REACHED;
			callEndAckCallback();
			break;
		case EOF_REACHED:
			this->errcode = errcode;
			state = EOF_REACHED;
			break;
		case STOPPED:
		case STOPPED_WHILE_CALLING:
		case STOPPED_WHILE_WAITING:
			P_BUG("May not call feedError() while in the STOPPED, STOPPED_WHILE_CALLING "
				"or STOPPED_WHILE_WAITING state");
			break;
		case PLANNING_TO_CALL:
			ctx->libev->cancelCommand(planId);
			planId = 0;
			this->errcode = errcode;
			state = EOF_REACHED;
			callEndAckCallback();
			break;
		default:
			P_BUG("Unknown state" << toString((int) state));
			break;
		}
	}

	/**
	 * Resume a stopped Channel.
	 */
	void start() {
		switch (state) {
		case IDLE:
		case CALLING:
		case PLANNING_TO_CALL:
		case WAITING_FOR_CALLBACK:
		case CALLING_WITH_EOF:
		case EOF_WAITING:
		case EOF_REACHED:
			break;
		case STOPPED:
			planNextActivity();
			break;
		case STOPPED_WHILE_CALLING:
			state = CALLING;
			break;
		case STOPPED_WHILE_WAITING:
			state = WAITING_FOR_CALLBACK;
			break;
		default:
			P_BUG("Unknown state" << toString((int) state));
			break;
		}
	}

	/**
	 * Stops a Channel. That is, do not call the callback even when there
	 * is data available. This continues until you call `start()`.
	 */
	void stop() {
		switch (state) {
		case STOPPED:
		case STOPPED_WHILE_CALLING:
		case STOPPED_WHILE_WAITING:
		case CALLING_WITH_EOF:
		case EOF_WAITING:
		case EOF_REACHED:
			break;
		case IDLE:
		case PLANNING_TO_CALL:
			state = STOPPED;
			if (state == PLANNING_TO_CALL) {
				ctx->libev->cancelCommand(planId);
				planId = 0;
			}
			break;
		case CALLING:
			state = STOPPED_WHILE_CALLING;
			break;
		case WAITING_FOR_CALLBACK:
			state = STOPPED_WHILE_WAITING;
			break;
		default:
			P_BUG("Unknown state" << toString((int) state));
			break;
		}
	}

	/**
	 * If the callback returned -1, then at some later point it must call this method
	 * to notify Channel how many bytes have been consumed.
	 */
	void consumed(unsigned int size, bool end) {
		assert(state != IDLE);
		assert(state != CALLING);
		assert(state != STOPPED);
		assert(state != STOPPED_WHILE_CALLING);
		assert(state != PLANNING_TO_CALL);
		assert(state != CALLING_WITH_EOF);
		assert(state != EOF_REACHED);

		buffer = MemoryKit::mbuf(buffer, size);

		switch (state) {
		case WAITING_FOR_CALLBACK:
			if (end) {
				goto end_reached;
			} else {
				planNextActivity();
			}
			break;
		case STOPPED_WHILE_WAITING:
			if (end) {
				goto end_reached;
			} else {
				state = STOPPED;
			}
			break;
		case EOF_WAITING:
			end_reached:
			state = EOF_REACHED;
			callEndAckCallback();
			break;
		default:
			P_BUG("Unknown state" << toString((int) state));
			break;
		}
	}

	OXT_FORCE_INLINE
	State getState() const {
		return state;
	}

	/**
	 * Returns whether this Channel accepts more input right now.
	 * There are two reasons why this might not be the case:
	 * either the callback isn't done yet, or an error had been fed.
	 * Use `hasError()` to check for the latter.
	 */
	OXT_FORCE_INLINE
	bool acceptingInput() const {
		return state == IDLE;
	}

	OXT_FORCE_INLINE
	bool hasError() const {
		return errcode != 0;
	}

	bool isStarted() const {
		return state != STOPPED && state != STOPPED_WHILE_CALLING && state != STOPPED_WHILE_WAITING;
	}

	OXT_FORCE_INLINE
	int getErrcode() const {
		return errcode;
	}

	bool ended() const {
		return state == CALLING_WITH_EOF || state == EOF_WAITING || state == EOF_REACHED;
	}

	OXT_FORCE_INLINE
	bool endAcked() const {
		return state == EOF_REACHED;
	}
};


} // namespace ServerKit
} // namespace Passenger

#endif /* _PASSENGER_SERVER_KIT_CHANNEL_H_ */
