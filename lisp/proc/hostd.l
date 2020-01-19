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
;;
;; hostd.l -- A Universal Host
;;
;; Incoming guest is served in a new spawned process.
;; Handshake and protocol negotiation happen in the new process.
;;
;; Process Arguments
;;
;; - port: listening port
;; - keypair: host identity file
;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

(define port (car args))
(define keypair (read (open-input-file (cadr args))))
(define uuid (pubkey->address (cdr keypair)))

(defmethod (run)
  ;; Take incoming guests and dispatch them onto independent processes.
  ;; Do not take too many clients in one go. let other processes run.
  (let loop [(n 10)] 
    (if (and (> n 0) (ready? incoming))
        (let ((client (read incoming)))
          (if (not client)
	      (error "No incoming client"))

	  ;; TODO we may need to do something about ip logging or filtering at this point
	  
	  ;; Start a reception process to handle incoming guest
          (define pid (spawn start-peerx (list (cdr client) keypair)))
	  ;; FIXME potential leak of client socket especially when we can't spawn
	  ;; new processes
          (if pid
              (set-process-socket pid (car client)))
	  
          (loop (- n 1))
          ))))


(set-process-name "hostd-\{(substring uuid 0 4)}/\{port}")
(define incoming (open-tcp-server "0.0.0.0" port))
(println "Host \{uuid} started at port \{port}")
