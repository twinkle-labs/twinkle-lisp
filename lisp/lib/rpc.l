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

;;----------------------------------------------------------------------
;; (make-rpc-man EVENTQ POST PROCESS)
;; Create a "Remote Procedure Call" manager.
;; Used inside a distributed process to handle rpc
;; handles timeout and rpc request/ack matching.
;; doesn't include communication and response
;; handling. so that the code can be reused in networked or
;; simulated environment.
;;
;; INPUT:
;; - EVENTQ: for scheduling
;; - POST: (post rcpt msg)
;;   for sending messages
;; - PROCESS: (process from req-id req-data req-time)
;;   Process a request. ACK is auto dispatched to ack handler
;;
;; (require 'event-queue)
;;
;; METHODS
;; - (send-request RCPT FROM DATA ACK-FN &optional TIMEOUT TIMEOUT-FN)
;;   
;; - (send-ack RCPT FROM REQ-ID DATA)
;;   after the user process the message, it may use this function to
;;   send back a response.
;; - (receive rcpt msg)
;;   called by message transfer agent.
;;
;; Perhaps we should hide REQ-ID from interfaces. process should have
;; a ack sender. that will automatically fills in the req-id.
;;
;; RCPT and FROM are addresses. But we don't have explict assumption
;; the address is handled by the POST. it's not actually understood by
;; rpc.
;;----------------------------------------------------------------------
(define (make-rpc-man eventq post process)

  (define pending-request-alist ())
  (define num-requests 0)
  (define num-acks 0)
  (define num-timeouts 0)
  (define num-responses 0)
  
  (defmethod (stat)
    `((req . ,num-requests)
      (ack . ,num-acks)
      (queue . ,(length pending-request-alist))
      (timeout . ,num-timeouts)
      (response . ,num-responses))
    )
  
  (define (random-id)
    (floor (* (random) 100000000)))

  (defmethod (send-request rcpt from req-data ack-fn &optional timeout timeout-fn)
    (set! num-requests (+ num-requests 1))
    ;; Set timeout to 30 seconds by default
    (if (not (number? timeout)) (set! timeout 30))

    (define req-id (random-id))
    (define expire-time (+ (eventq 'now) timeout))
    (add-to-list! pending-request-alist (list req-id expire-time ack-fn timeout-fn))
    
    (post rcpt
    	(list 'req
    	from
    	req-id
    	req-data
    	(eventq 'now))))

  ;; Call by our hosting process 
  ;; FORMAT OF ACK
  ;; (ack FROM REQ-ID RESPONSE ACK-TIME)
  (defmethod (send-ack rcpt from req-id response)
    (set! num-responses (+ num-responses 1))
  	(post rcpt (list
	  		'ack 
	  		from 
	  		req-id
	  		response
	  		(eventq 'now))))

  ;; Call by the message sender on a new message destined to us
  (defmethod (receive rcpt msg)
  	(if (or (not (list? msg)) (null? msg))
  		(return))
  	(cond
  		((eq? (car msg) 'ack)   
  			(apply process-ack (cdr msg)))
  		((eq? (car msg) 'req)
                 (let [(from (cadr msg)) (req-id (caddr msg))]
                   (apply process (cons
                                   (lambda (response)
                                     (send-ack from rcpt req-id response)
                                     )
                                   (cdddr msg)))
                   ))
  		((else (error "Invalid message")))))

  (define (process-ack from req-id response ack-time)    
    (define req (assoc req-id pending-request-alist))
    (if (not req) 
    	(begin 
         ;;(println "ACK:" (eventq 'now) ":Ignore " req-id)
         (return)))
    (set! num-acks (+ num-acks 1))
    ;;(println "RPC: " (eventq 'now) ": ack#" req-id " => " response)
    (define ack-fn (caddr req))
    (if (procedure? ack-fn)
      (ack-fn response))
    (set! pending-request-alist 
      (remove 
        (lambda (a) (eq? a req)) 
        pending-request-alist)))

  (define (process-timeout-requests l)
    (if (null? l) (return))
    (set! num-timeouts (+ num-timeouts 1))
    (define timeout-fn (nth (car l) 3))
    (if (procedure? timeout-fn)
        (timeout-fn))
    (process-timeout-requests (cdr l)))

  (define (remove-expired-requests)
    (define now (eventq 'now))
    ;; u: active requests, v: timeout requests
    (let loop ((l pending-request-alist) (u ()) (v ()))
    	(if (null? l)
    		(begin
    			(set! pending-request-alist u)
    			(process-timeout-requests v)
    			(return)))
    	(define a (car l))
    	(if (> now (cadr a))
    		(loop (cdr l) u (cons a v))
    		(loop (cdr l) (cons a u) v))))

  (define (tick)
  	;;(println "RPC TICK " (eventq 'now))
    (remove-expired-requests)
    (eventq 'sched 1 tick))

  (eventq 'sched 1 tick)

  (this))


(define (test-rpc)
	(define eventq (make-event-queue))
	(define (process from req-id req-data req-time)
		(println "Process Request:" from ":" req-id ":" req-data ":" req-time)
		(rpc-man 'send-ack from "BOB" req-id "ECHO")
		)
	(define (post rcpt msg)
		(println "Post:" (eventq 'now) ":" rcpt ":" msg)
		(eventq 'sched 5 (lambda () (rpc-man 'receive (list 
			'ack rcpt (nth msg 2) "GOT IT" (eventq 'now)

			) )))
		)
	(define rpc-man (make-rpc-man eventq post process))

	(rpc-man 'send-request "DUMMY" "BOB" "HAHA" (lambda (response)
			(println  "Got ack:" (eventq 'now) ":" response)
		))

	(eventq 'sched 10 (lambda ()
		(rpc-man 'receive (list
			'req "DUMMY" 3333 "Hello" (eventq 'now)
			))
		))

	(eventq 'run-to 60)
	(println eventq)

	)

(define (test-rpc2)
	(define eventq (make-event-queue))
	(define (process from req-id req-data req-time)
		(println "Process Request:" from ":" req-id ":" req-data ":" req-time)
		(rpc-man 'send-ack from "BOB" req-id "ECHO")
		)
	
	(define (post rcpt msg)
		(println "Post:" (eventq 'now) ":" rcpt ":" msg)
		(eventq 'sched (+ 1 (* (random) 5)) (lambda () (rpc-man 'receive msg )))
		)

	(define rpc-man (make-rpc-man eventq post process))

	(define (talk)
	(rpc-man 'send-request "DUMMY" "BOB" "HAHA" (lambda (response)
			(println  "Got ack:" (eventq 'now) ":" response)
			(talk)
		))
	)

	(talk)


	(eventq 'run-to 60)
	)

