;;    
;; Copyright (C) 2020, Twinkle Labs, LLC.
;;
;; This program is free software: you can redistribute it and/or modify
;; it under the terms of the GNU Affero General Public License as published
;; by the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.
;;
;; This program is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU Affero General Public License for more details.
;;
;; You should have received a copy of the GNU Affero General Public License
;; along with this program.  If not, see <https://www.gnu.org/licenses/>.
;;

(define (make-clock)
  (define value 0)
  (define last-start false)
  
  (defmethod (tell)
    (if last-start
	(+ value (- (microtime) last-start))
	value))

  (defmethod (set-value v)
    (set! value v))

  (defmethod (start)
    (set! last-start (microtime)))

  (defmethod (stop)
    (set! value (tell))
    (set! last-start false))

  (this))



;; (MAKE-EVENT-QUEUE)
;; Every distributed process needs one event queue
;; to track its pending jobs.
;; Emulation
;;   - Configurable clock.
;;     The time is not necessarily in real time.
;;     We can pretend each queue runs on its own
;;     machine.
;;  - 
(define (make-event-queue &optional clk)

  (if (not clk)
      (set! clk (make-clock)))

  (defmethod (now)
    (clk 'tell))
  
  (define tq (make-timer-queue now))  

  (defmethod (stat)
    (tq 'stat)
    )

  ;; used for real 
  (defmethod (run)
    (tq 'run))
  
  ;; Run jobs that are due before now+duration.
  ;; Advance clock by duration
  (defmethod (run-to time)
    (clk 'start)
    (let loop ()
      (define next-interval (tq 'run))
      (if (and (> next-interval 0) (< (+ (now) next-interval) time))
        (begin
          (clk 'set-value (+ (now) next-interval))
          (loop)
          )
        )
    )
    (clk 'stop)
    (if (< (clk 'tell) time)
        (clk 'set-value time)))

  (defmethod (sched interval fn)
    (tq 'add interval fn)
    )

  (this))

