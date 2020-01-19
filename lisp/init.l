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

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; init.l --  TWK Initialization
;; Run as the first process.
;; Load site-init.l to configure application.
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

(println "*** Twinkle Lisp 2020 ***")

(set-logging-level 1)

(verbose "*dist-path*=" *dist-path*)
(verbose "*var-path*=" *var-path*)

(mkdir "\(*var-path*)")
(mkdir "\(*var-path*)/data")
(mkdir "\(*var-path*)/cache")
(mkdir "\(*var-path*)/cache/upload")

(define load-path 
  (list 
   "\(*dist-path*)/lisp"))


(load "lib/lib.l")
(load "lib/sqlite3.l")
(load "lib/util.l")
(load "lib/timer.l")
(load "lib/proc.l")
(load "lib/request-queue.l")
(load "lib/peer.l")
(load "lib/httpd.l")

(if (dir-exists? "\(*dist-path*)/site-lisp")
    (set! load-path (cons "\(*dist-path*)/site-lisp" load-path)))

(if (file-exists? "\(*dist-path*)/site-lisp/site-init.l")
    (load "site-init.l"))

(define (start-rexec sid ip port uid req done)
  ;; sid -- server uuid
  ;; ip -- server ip, string
  ;; port -- server port, integer
  ;; uid: user keypair for authorization, could be empty
  ;;      if the request is public
  ;; req: (<end-point>  <method> &rest params)
  ;; done: callback when result is returned.
  (define rq (current-request-queue))
  (define req-id (random))
  (rq 'add-request req-id (time) done)
  (spawn start-peer
	 (list sid ip port uid 'rexec
	       :execute req
	       :ack (list (get-pid) req-id)
	       )))


(defmethod (launch name &rest args)
  (spawn usr-process-init (list name args))
  )

(defmethod (rexec server-uuid server-ip server-port userid name &rest args)
  (define kp
    (if (null? userid)
	()
	(read (open-input-file userid))))
  (spawn start-peer
	 (list server-uuid server-ip server-port
	       kp 'rexec
	       :execute (cons name args)
	       )))

(defmethod (exec name &rest args)
  (define x ((lambda ()
	       (load "exec/\{name}.l")
	       (this))))
  (apply x  args))

(defmethod (test case-name &rest args)
  (load "tests/test-\{case-name}.l"))

(defmethod (help &optional command)
  (cond
   
   ((eq? command "launch")
    (println "Usage: twk launch <process-name> <options>
Start a process, with a list of options.
A process is loaded from a file under 'proc/' directory with the name '<process-name>.l'.
"))

   ((eq? command "exec")
    (println "Usage: twk exec <program-name> <method> <params>
Load a file under 'exec/' folder with <name>, and execute the <method> with 
provided parameters.
"))

   ((eq? command "rexec")
    (println "Usage: twk rexec <server-info> <key-pair-file> <program-name> <method> <params>
Remote execution.
Send a request to a remote server, for running a remote program invoked with method and params provided.
The result of that method will be returned and written on current console output.
"))

   ((eq? command "test")
    (println "Usage: twk test <name>
Load a test case under tests/ directory."))

   (else 
    (println "Usage: twk <command> <options>
Available commands: 
 help launch exec rexec test 
Type `twk help <command>' for more help.
"))
   
   ))

;; The Main Program Entry.
(let []
  (define *args* (get-args))
  (verbose "*args*=" *args*)
  (if (null? *args*)
      (help)
      (apply (this) *args*))

  (exit))



