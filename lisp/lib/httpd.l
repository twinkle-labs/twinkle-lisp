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

(define web-root "\(*dist-path*)/web")

(define (get-query-params req)
  (define q (assoc 'query req))
  (if q (cdr q) ()))

(define (http-request-param req name)
  (define x (assoc name (get-query-params req)))
  (if x (cdr x) false))

(define (http-request-get-header req header-field)
  (define a (assoc header-field (cdr (assoc 'headers req))))
  (if a (cdr a) false)
  )

(define (http-request-get-cookies req)
  (define k (http-request-get-header req 'Cookie))
  (if k
      (map ^{[x]
	     (define pos (string-find x "="))
	     (if pos
		 (cons (string->symbol (substring x 0 pos))
		       (substring x (+ pos 1)))
		 ())
	     } (split k "; "))
      ()))

(define (http-get-session req)
  (define cookies (http-request-get-cookies req))
  (define access-token false)
  (if (assoc 'access-token cookies)
      (set! access-token cookies:access-token)
      (set! access-token (http-request-param req 'access-token)))
  (if (not access-token)
      (return false))
  (define s (global-session-db 'find "token" :id access-token))
  (if (null? s) false s))

(define (atom->json a)
  (cond
   ((string? a) (string-quote a))
   ((eq? a undefined) "null")
   ((eq? a true) "true")
   ((eq? a false) "false")
   ((symbol? a) (string-quote (symbol->string a)))
   ((number? a) (number->string a))
   (else (error "can not convert to json string"))))

(define (key->json k)
  (if (symbol? k)
      (concat "\"" (symbol->string k) "\":")
      (error "invalid key")))

(define (alist->json al)
  (concat "{"
          (join (map (lambda (a)
                       (concat (key->json (car a)) (atom->json (cdr a))))
                     al) ",")
          "}"))

(define (list->json l)
  (concat "["
          (join l ",")
          "]"))



;; https://developer.mozilla.org/en-US/docs/Web/HTTP/Basics_of_HTTP/MIME_types/Complete_list_of_MIME_types
(define mime-types
  '(("html" . "text/html; charset=UTF-8")
    ("png"  . "image/png")
    ("pdf"  . "application/pdf")
    ("mp3"  . "audio/mp3")
    ("mp4"  . "video/mp4")
    ("jpeg" . "image/jpeg")
    ("css"  . "text/css; charset=UTF-8")
    ("js"   . "application/javascript; charset=UTF-8")
    ("json" . "application/json; charset=UTF-8")
    ("jpg"  . "image/jpeg")))
    

(define (path-extension path)
  (define i (string-find-backward path "."))
  (if i (substring path (+ i 1))))

(define (content-type path)
  (define t (assoc (path-extension path) mime-types))
  (if t (cdr t)  "application/octet-stream"))


(define (on-http-request req http-input http-output)

  ;; FIXME if the req has connection close, then we should not write
  ;; and if we have ready served a long time, we should close 
  (define (keep-alive)
   ;; (if (eq? (http-request-get-header req 'Connection) "keep-alive")
        (print "Connection: Keep-Alive\r\n")
        (print "Keep-Alive: timeout=30, max=100\r\n"))

  (define (http-not-found fpath)
    (define msg (concat "File Not Found\n"))
    (with-output http-output
                 (print "HTTP/1.1 404 Not Found\r\n")
                 (print "Content-Length: " (length msg) "\r\n")
                 (print "Content-Type: text/plain; charset=UTF-8\r\n")
                 (keep-alive)
                 (print "\r\n")
                 (print msg)))

  (define (http-send-message msg)
    (with-output http-output
                 (print "HTTP/1.1 200 OK\r\n")
                 (print "Content-Length: " (length msg) "\r\n")
                 (print "Content-Type: text/plain; charset=UTF-8\r\n")
                 (print "Connection: keep-alive\r\n")
                 (print "Keep-Alive: timeout=30,max=100\r\n")
                 (print "\r\n")
                 (print msg)))

  (define (http-send-json text)
    (with-output http-output
                 (print "HTTP/1.1 200 OK\r\n")
                 (print "Content-Length: " (length text) "\r\n")
                 (print "Content-Type: application/json; charset=UTF-8\r\n")
                 (keep-alive)
                 (print "\r\n")
                 (print text)))

  (define (http-send-alist l)
    (define text (concat l))
    (with-output http-output
                 (print "HTTP/1.1 200 OK\r\n")
                 (print "Content-Length: " (length text) "\r\n")
                 (print "Content-Type: text/plain; charset=UTF-8\r\n")
                 (keep-alive)
                 (print "\r\n")
                 (print text)))

  (define (http-send-partial-file path total range)
    (define r (http-parse-range range total))
    (if (not r)
        (error "Bad range:" range))
    (define offset (car r))
    (define size (cdr r))
    (with-output http-output
                 (print "HTTP/1.1 206 Partial Content\r\n")
                 (print "Content-Range: bytes " offset "-" (+ offset size -1) "/" total "\r\n")
                 (print "Content-Length: " size "\r\n")
                 (keep-alive)
                 (print "\r\n"))
    (define f (open-input-file path))
    (seek f offset)
    (pump f http-output size)
    (close f))

  (define (http-send-file path &optional name)
    (define range (http-request-get-header req 'Range))
    (define total (filesize path))
    (if range
        (return (http-send-partial-file path total range)))
    (with-output http-output
                 (print "HTTP/1.1 200 OK\r\n")
                 (print "Content-Length: " total "\r\n")
                 (if name
                     (begin
                       (print "Content-Type: "  (content-type name)  "\r\n")
                       (print "Content-Disposition: attachment; filename=\"" name "\"\r\n"))
                     (print "Content-Type: "  (content-type path)  "\r\n"))
                 (print "Accept-Ranges: bytes\r\n")
                 (keep-alive)
                 (print "\r\n"))
    (if (eq? (car req) 'GET)
        (pump (open-input-file path) http-output)))

  (define (http-send-partial-from-port input total range &optional name type)
    (define r (http-parse-range range total))
    (if (not r)
        (error "Bad range:" range))
    (define offset (car r))
    (define size (cdr r))
    (with-output http-output
                 (print "HTTP/1.1 206 Partial Content\r\n")
                 (print "Content-Range: bytes " offset "-" (+ offset size -1) "/" total "\r\n")
                 (print "Content-Length: " size "\r\n")
		 (if type
                     (print "Content-Type: \{type}\r\n")
		     (if name
                       (print "Content-Type: "  (content-type name)  "\r\n")
		       (print "Content-Type: application/octet-stream\r\n")))
                 (if name
		     (print "Content-Disposition: attachment; filename=\"" name "\"\r\n"))
		 
                 (keep-alive)
                 (print "\r\n"))
    (seek input offset)
    (pump input http-output size))
  
  (define (http-send-from-port input total &optional name type)
    (define range (http-request-get-header req 'Range))
    (if range
        (return (http-send-partial-from-port input total range name type)))
    (with-output http-output
                 (print "HTTP/1.1 200 OK\r\n")
                 (print "Content-Length: " total "\r\n")
		 (if type
                     (print "Content-Type: \{type}\r\n")
		     (if name
                       (print "Content-Type: "  (content-type name)  "\r\n")
		       (print "Content-Type: application/octet-stream\r\n")))
                 (if name
		     (print "Content-Disposition: attachment; filename=\"" name "\"\r\n"))
                 (print "Accept-Ranges: bytes\r\n")
                 (keep-alive)
                 (print "\r\n"))
    (if (eq? (car req) 'GET)
        (pump input http-output)))

  
  (define (http-redirect path)
    (with-output http-output
                 (print "HTTP/1.1 301 Moved Permanently\r\n")
                 (print "Location: " path "\r\n")
                 (keep-alive)
                 (print "\r\n")))

  (define (http-try-file fpath &optional name)
    (if (file-exists? fpath)
        (http-send-file fpath name)
        (begin 
          (println "file not found:" fpath)
          (http-not-found fpath))))

  (define (load-module filename)
    ((lambda ()
       (load filename)
       (this))))

  (define (run-api path req)
    (define offset (string-find-backward path "/"))
    (define method (substring path (+ offset 1)))
    (define file-path (substring path 0 offset))
    (define args
      (cons
       (string->symbol method)
       (cons req
             (get-query-params req))))
;;    (println "args=" args)
    (apply (load-module "web\{file-path}.l") args)
    )

  (define (process-request req)
    (define path (alist-get req 'path))
    (define fpath (concat web-root path))
    (cond
     [(file-exists? fpath)
      (http-send-file fpath)
      ]
     [(dir-exists? fpath)
      (if (suffix? fpath "/")
	  (http-try-file (concat fpath "index.html"))
	  (http-redirect (concat path "/")))
      ]
     [(prefix? path "/api/")
        (run-api path req)]
     [else
      (load "web/index.l")]))

  (process-request req)
  (flush http-output)
  ) ;; End of on-http-request


(define (start-http-server ip port)
  (set-process-name "httpd")
  (define incoming (open-tcp-server ip port))
  (if incoming
      (send-message -1 (list 'httpd-started port))
      (begin
	(send-message -1 (list 'httpd-failed))
	(error "Failed to start")))
  (defmethod (run)
    (let loop [(n 10)]
      (if (and (> n 0) (ready? incoming))
          (let ((client (read incoming)))
            (if (not client) (error "No incoming client"))
	    (define sockfd (car client))
	    (if (or (not (integer? sockfd)) (< sockfd 0))
		(error "Invalid socket"))
            (define pid (spawn start-http-client (list (cdr client))))
	    ;; FIXME possible leak of socket
            (if pid
                (set-process-socket pid (car client)))
            (loop (- n 1))
            ))))

  (this)
  )


(define (start-http-client client-ip)
  (define self (this))
  (set-process-name "http-client")
  (define in (open-socket-input))
  (define out (open-socket-output))
  (define web-socket-mode false)


  (define (websocket-upgrade req)
    (set! web-socket-mode true)
      ;;FIXME Make sure path is valid
    (define path (concat "web" (alist-get req 'path) ".l"))
    (define k (http-request-get-header req 'Sec-WebSocket-Key))
    (define guid "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
    (define a (base64-encode (sha1 (concat k guid))))
    (with-output out
                 (print "HTTP/1.1 101 Switching Protocols\r\n")
                 (print "Upgrade: WebSocket\r\n")
                 (print "Connection: Upgrade\r\n")
                 (print "Sec-WebSocket-Accept: \{a}\r\n\r\n")
                 )
    (eval `(load ,path) self)
    (websocket-init req)
    )

  (defmethod (run)
    (run1)
    )

  (define (run1)
    (let loop []
      (if (ready? in)
          (cond
           [web-socket-mode
	    (if (websocket-receive (websocket-read in))
		(loop)
		(exit))]
           [else
            (let [(req (http-read in))]
              (if req
                  (begin
                    (verbose (get-pid) ":" req)
                    (if (eq? (http-request-get-header req 'Upgrade) "websocket")
                        (websocket-upgrade req)
                        (on-http-request req in out))
                    (loop))
                  (exit)))]))))

  (this))


