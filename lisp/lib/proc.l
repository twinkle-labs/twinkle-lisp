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

;; FIXME we should not have a explicit env
;; parameter here, we should assume
;; that the process's body runs 
;; in the root environment, rather than
;; create its own.
(define (process-init env)
  (define init (eval '*init* env))

  ;; Optional request queue
  (eval `(define *rq* false) env)

  (define mbox (open-mbox))
  (if (not (ready? mbox))
      (error "process-init: Empty mbox"))
  (define msg (read mbox))
  (define main (catch (apply init msg)))
  (cond
   [(env? main)
    (eval `(define *main* (quote ,main)) env)
    (eval `(define *mbox* (quote ,mbox)) env)
    ]
   [else
    (if (error? main)
        (send-message (get-parent-pid) (list 'on-child-abort (get-pid) main)))
    (exit)])
  )


(define (repl-from-string s env)
  (define p (open-input-buffer s))
  (let loop [(o (read p)) (r ())]
    (if (eq? o *eof*) (return r))
    (set! r (eval o env))
    (if (not (eq? r 'undefined))
        (println r))
    (loop (read p) r)))


;;----------------------------------------------------------------------
;; (process-run <env>)
;;
;; Every process's executing start point.
;; This is the safe environment.
;; We check for common system messages and handle
;; them, and we can run priviledged code here.
;; Then we pass on user messages to the process's
;; user code, where the code can not access
;; priviledged data.
;; The <env> is the process's start environment,
;; but that's invisible to process's user code,
;; because user code extends its intial procedure's
;; environment, but not the process's start environment.
;;----------------------------------------------------------------------
(define (process-run env)
  (define main (eval '*main* env))
  (define mbox (eval '*mbox* env))

  (define (receive msg)
    (define name (car msg))
;    (println "Process " (get-pid) ":" msg)
    (match msg
     [(request pid req-id req-message)
      (if (method? 'on-request main)
          (apply main (list 'on-request req-message
                            ^{[x](send-message pid (list 'did-request req-id x))}))
          (send-message pid (list 'did-request req-id (receive req-message))))]
     [(did-request req-id response)
      ((eval '*rq* env) 'did-request req-id response)      
      ]
     [(quit)
      (exit)]
     [else
      (if (method? name main)
	  (apply main msg))])
    )

  (define x
    (catch
     (let loop ()
       (cond [(exited?) ]
             [(and (> (get-timeout) 0) (< (get-timeout) (microtime)))
              (set-timeout 0)
              (if (method? 'timeout main) (main 'timeout))
              (loop)]
             [(ready? mbox) (receive (read mbox)) (loop)]
             [(method? 'run main) (main 'run)] ;; Quit after run
             ))))

  (if (error? x)
      (begin
        (send-message (get-parent-pid) (list 'on-child-abort (get-pid) x))
        (exit)))
  
  true)


;; End of PROCESS-RUN

(define (current-request-queue)
  (define env (get-process-environment))
  (define rq (eval '*rq* env))
  (if (not rq)
      (let [(q (make-request-queue))]
	(set! rq q)
	(eval `(set! *rq* (quote ,q)) env)
	))
  rq)

(define (send-request pid message done)
  ((current-request-queue) 'send pid message done))

(define (usr-process-init name args)
  (set-process-name (concat "proc/" name))
  (load (concat "proc/" name ".l"))
  (this))

(define (start name &rest args)
  (spawn usr-process-init (list name args)))
