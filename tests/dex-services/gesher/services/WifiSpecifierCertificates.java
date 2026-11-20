/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.wifi.WifiEnterpriseConfig;
import java.io.ByteArrayInputStream;
import java.security.KeyFactory;
import java.security.PrivateKey;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.Base64;

/** Public test-only certificate material, never an installed credential or trust anchor.
 * Generated with cryptography: fixed P-384 scalars 560101/560102; generated RSA keys.
 * Validity 2020-01-01..2040-01-01; CA/pathLen1 and end-entity BasicConstraints.
 * These fixtures exercise actual certificate parsing and Suite-B classification.
 */
final class WifiSpecifierCertificates {
    private static final String EC384_CA =
            "MIIBpjCCASygAwIBAgIDCIvlMAoGCCqGSM49BAMDMDExLzAtBgNVBAMMJkdlc2hlciBwdWJsaWMgdGVzdC1vbmx5IFdpLUZpIEVD" +
            "Mzg0IENBMB4XDTIwMDEwMTAwMDAwMFoXDTQwMDEwMTAwMDAwMFowMTEvMC0GA1UEAwwmR2VzaGVyIHB1YmxpYyB0ZXN0LW9ubHkg" +
            "V2ktRmkgRUMzODQgQ0EwdjAQBgcqhkjOPQIBBgUrgQQAIgNiAATYC+dSU8lnLHR34nU44MXFERfKodOts8geH7Lljhx5JCf5KjES" +
            "r+1GluWjalM6CzVtGE2pVuBlVc2EwZtDAXCPT0NWIgBAZtgsQ2+ZyfoBuaSSdo1sN0yrQGXGfFkKFcCjFjAUMBIGA1UdEwEB/wQI" +
            "MAYBAf8CAQEwCgYIKoZIzj0EAwMDaAAwZQIwbKVVmy5BaHbx9aA7b8b/Gs7HhV57mdoUFc80CMMtDtXxDvp6/IKGp7DDWC8HPBee" +
            "AjEAiKTbOYigPeewdGKcYJ14Vw/eQcMkEO7PSIastZapVfeBWRaD/84COpMx+swxWdOL";
    private static final String EC384_CLIENT =
            "MIIBpDCCASqgAwIBAgIDCIvmMAoGCCqGSM49BAMDMDExLzAtBgNVBAMMJkdlc2hlciBwdWJsaWMgdGVzdC1vbmx5IFdpLUZpIEVD" +
            "Mzg0IENBMB4XDTIwMDEwMTAwMDAwMFoXDTQwMDEwMTAwMDAwMFowNTEzMDEGA1UEAwwqR2VzaGVyIHB1YmxpYyB0ZXN0LW9ubHkg" +
            "V2ktRmkgRUMzODQgY2xpZW50MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEQLQR7e5gMPSzQEvZpCllHVOPkAcRWUSxO7ZElXrbE7pT" +
            "ENYk2hNJwf2odEBGJ/ThhxnO/rJJINMFICjyjUsAtB7tOwDTKvU2BNW76mMpQnyBbTb4tew920vNLSIYrfpRoxAwDjAMBgNVHRMB" +
            "Af8EAjAAMAoGCCqGSM49BAMDA2gAMGUCMBro6olOVAnO4ahWv0lZz/lpqotAn4TJLo3YzE4QzwtwBkpregw3tGWLk6jeGrx/ZQIx" +
            "AJAnqejDcZOG/e9DGG7I2F/fWoy11ty8kpSIw+qnGVAdnFPEqewAHhNc4eAgJXCy6w==";
    private static final String EC384_KEY =
            "MIG2AgEAMBAGByqGSM49AgEGBSuBBAAiBIGeMIGbAgEBBDAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
            "AAAAAAAIi+ahZANiAARAtBHt7mAw9LNAS9mkKWUdU4+QBxFZRLE7tkSVetsTulMQ1iTaE0nB/ah0QEYn9OGHGc7+skkg0wUgKPKN" +
            "SwC0Hu07ANMq9TYE1bvqYylCfIFtNvi17D3bS80tIhit+lE=";
    private static final String RSA3072_CA =
            "MIID+TCCAmGgAwIBAgIDCIvlMA0GCSqGSIb3DQEBDAUAMDMxMTAvBgNVBAMMKEdlc2hlciBwdWJsaWMgdGVzdC1vbmx5IFdpLUZp" +
            "IFJTQTMwNzIgQ0EwHhcNMjAwMTAxMDAwMDAwWhcNNDAwMTAxMDAwMDAwWjAzMTEwLwYDVQQDDChHZXNoZXIgcHVibGljIHRlc3Qt" +
            "b25seSBXaS1GaSBSU0EzMDcyIENBMIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEArVY+FltBmWFHQa7syIDat6ggWM2b" +
            "DgHQxK1PxJkrU7kzX5Z0NUjZdh2j9sEBFjWEw9FhK3Q2+R8AH2QStnAQXxi8vZLfAmQIKO4bi8HLYJ0otZYCqe2OS6yFYtS+ZbHO" +
            "LSDP90BKS0GJvPt/S58HnVYT5VjAj10cDifEcJ7FqLgrL/YDcAemFFlFhHyjlcd7kRyB0qYpUl/Qw7I3JE9JAyJhpC9tkpcsb72v" +
            "Jyn9et4YC08ZHj4rT7Hon7x9lkF6w6lauZ8ID7FGakYQD8UXrTxRN0ea6lY4p2isFgo6TZb9Gyc0G+NbD8mv4sJa1ZAGuI3OkRLs" +
            "F/c9yBEuEwc47+FvO3kAQJm3N8kTlQMIBsMHf47LFDhktdNGmbtEyVlVy9BQ4MaWH/+z7xJpCRyb44NxqF3C2N9nGwlXjS6GdMVJ" +
            "Lg0zndjTxqzjI1pNsU9hMCpQT9D/U5VksjZk5d62CEOx3RMezvfp2VGz23t5YtiOFVgjL+qZa685S0fBpDUlAgMBAAGjFjAUMBIG" +
            "A1UdEwEB/wQIMAYBAf8CAQEwDQYJKoZIhvcNAQEMBQADggGBADxJw39UYMMQR1Ce15eYCHPbdIN1behzO4BaIh1p9yyqOTW+7ube" +
            "D/Iy/VRn7GFVmkwCaIBT3+bcIcXzqAOKtWAQSC/1gUGXfjySu/wGZbkPad+zz0rXBGGuSVVAYMq5MrUbrgnGawiQMir0GEd6vYMk" +
            "v3GmIzuCRrm6WCt+IVrRAlMf/dp+xgX3jpr/0vbLL23CFuJLFLbCspe4/cmu8VU0+MM1mGkbd1rdKOEJPoD1r5cYq+rqeSk8zrmp" +
            "O71UEQMIBxGIVOzJNv3BcNayldR1nyAubia/5EHyeUFZSEPX4gyYlKSwiuhcjHJrGUKGG3yiuvnftPIoREPWx1IyKXLplszo6APZ" +
            "H0oN6h0oWnZMVBwEVAY/4ZZorVFf6WmFb51KCpiCNZb0Neogu/SWJyzd0EG3W8edVVr375a6xzwmIW95DHYgehPshxXfzYg4NOAJ" +
            "jHDW9wi1/wO9HdqLalJSaE6Div4Rx37IDjpE5PqSKS5b1BElESGzCNKb/yOr6A==";
    private static final String RSA3072_CLIENT =
            "MIID9zCCAl+gAwIBAgIDCIvmMA0GCSqGSIb3DQEBDAUAMDMxMTAvBgNVBAMMKEdlc2hlciBwdWJsaWMgdGVzdC1vbmx5IFdpLUZp" +
            "IFJTQTMwNzIgQ0EwHhcNMjAwMTAxMDAwMDAwWhcNNDAwMTAxMDAwMDAwWjA3MTUwMwYDVQQDDCxHZXNoZXIgcHVibGljIHRlc3Qt" +
            "b25seSBXaS1GaSBSU0EzMDcyIGNsaWVudDCCAaIwDQYJKoZIhvcNAQEBBQADggGPADCCAYoCggGBANeR+lVsgx+kOURjOiWvfSED" +
            "7+t9DEXq3OH0vJ/8V8/QdO0roKo1WM46pCvTgu/9BvDZvqv2ZWPDYM548vFXUZAecVd+zjCVgK4VFj1L+SxJMzsydStfpSKOfnO1" +
            "vWyXvNJSuO7qnQSndfvTBrXzpKlK6sRTng/lpG5uTqYXv7Mi34GvwYrs7P2s2rIHhSEmw+z8guqFnDg+TbDMJM4p70QSSQqpwMez" +
            "P51iCyAl68wCLT5K1OQg17DhZaoNmli5hVzAtBv6dN7Qkq3pXzde/Jbx9uMHotsE4e48SiigktcsiKI/ncczaHhTpxeyIGYzA37q" +
            "sPi4bz+5fEU14xw4HGEW4OEU7dIIpr+KT+fMMbGa5gik9Rw5BCm3NmSNjYhOCbAUI85dl2kRXb8uK8s6cZ8fcfWSSQFtPcAtFGXj" +
            "Tf01H9cFsfgDiN9winJR+j/NnjRHfdW3OA6waG3FZ8jES/CFc91QJFO3h1ANu3ptaySEZA5SXxZ9dwPGtlwISnwq1QIDAQABoxAw" +
            "DjAMBgNVHRMBAf8EAjAAMA0GCSqGSIb3DQEBDAUAA4IBgQAv4gKOZKUIfH0m8I4pqwapNfxNZx7lfZ/fmUzRuZSWsbCGGzIQQd8j" +
            "I3YdfJTrNlLUL3yGdw9fOmB53zpeHxqWkgGgd/EGY66UsyS9qMFQ3hrOzEabNiRARD/TUII2xL3decGdSpg+RR3uWoo4NCuC2g2i" +
            "X8Gj06nbe+GWMOEwrYcMYUEsTJCXRoh2Gv/sYCc0fcELeaTH85QGFmI1LCL8OuCFzwfVsoQwoVxS5Y6LfLBwo2f7N3bPBHBxnWZI" +
            "u7ptZIr/3eigi4RInMmqcAoFfhILAivKH+LBnjCv+P+efK4prifaU16SZPVmn1/CHDQkas5HeGrvq/Yj7aBJoTG4JFqrKx3kk8gf" +
            "/QcJsUbWK1LiWAGNMt7KRlMib4VM6RqgxLo7oS43s1vq3X99Vb97u6ixr+yvhUY4PBYBX6KHi2RcEDkapFFaGc5eRiB/i4U0jzFV" +
            "AuNkd6xM7kveng8u3tXN1ODcyTE1rHDwAYwaNaruMXQT7qizWFHCdfmCwpI=";
    private static final String RSA3072_KEY =
            "MIIG+wIBADANBgkqhkiG9w0BAQEFAASCBuUwggbhAgEAAoIBgQDXkfpVbIMfpDlEYzolr30hA+/rfQxF6tzh9Lyf/FfP0HTtK6Cq" +
            "NVjOOqQr04Lv/Qbw2b6r9mVjw2DOePLxV1GQHnFXfs4wlYCuFRY9S/ksSTM7MnUrX6Uijn5ztb1sl7zSUrju6p0Ep3X70wa186Sp" +
            "SurEU54P5aRubk6mF7+zIt+Br8GK7Oz9rNqyB4UhJsPs/ILqhZw4Pk2wzCTOKe9EEkkKqcDHsz+dYgsgJevMAi0+StTkINew4WWq" +
            "DZpYuYVcwLQb+nTe0JKt6V83XvyW8fbjB6LbBOHuPEoooJLXLIiiP53HM2h4U6cXsiBmMwN+6rD4uG8/uXxFNeMcOBxhFuDhFO3S" +
            "CKa/ik/nzDGxmuYIpPUcOQQptzZkjY2ITgmwFCPOXZdpEV2/LivLOnGfH3H1kkkBbT3ALRRl4039NR/XBbH4A4jfcIpyUfo/zZ40" +
            "R33VtzgOsGhtxWfIxEvwhXPdUCRTt4dQDbt6bWskhGQOUl8WfXcDxrZcCEp8KtUCAwEAAQKCAX8p0n2+v6QGKiucw6OEVtzv4CkB" +
            "MZbapGDgyu/Nb3zvKdeFgUaqaa8mU4+GRoLLYfZMGUfkv5OWx1EAvcu5sYx2vsaT3bDNvDOQPTG+B/9PmDlYQscjTcBmnpPp3wM2" +
            "Y48+lCNM4YzqWj2JnbyvkEAMmCbz7++6eyhrmaWok52deNE5V9/faW07yff5PNGXR5i7YZdZ6JIRAgVl8jUxykZL7pWSfAdADIag" +
            "V2UmgjTntNm85se8JU2zWLILS+2l8607eGAF/SlUub2z+iU85+ntyn1GMZdOUSisMdQa9Sqkc2mZ6jWaAeynOOeVhYIdgvBYLpio" +
            "o5E9A1yaAhHTsztRpBZArD1AvLwH5AablZfFCz60cHbcLSHGPUUZ3OwxUhOI/1bhWzQicWcsqolfjWdF+sn6KObtBGy1SqKqtzK6" +
            "DKg+hclgu+g7axHvf/Skf+ZH/84g9OXx/bdsDv+U0axN3oXD8H3L/WGns6FEqI3MdlP1S3pDXSQkpNSyvr05AQKBwQDw5GMTjYgw" +
            "3crrnVAYebOk56RgLvZQHKNtDzViE8+CQyGqsfytQdUs6JtiICVuuGMNq8jxnNUoI7aXI3Bx3yFNj6v/b3E4Q1gAVFakxjlNZdH9" +
            "3Qh8E84yugn9TRXB/ApPbTsU7qTZ4uIbelMJppr8K6hqlmSLvBU74wY2vcExJQ4RC713+77Udk6TIEVqcQGfO3I5mMojRVJc6OTB" +
            "1R2dvOvEcOohrbNMkE2Pg6QzQhS4ebC9kNwzjTzfvvOWjMECgcEA5RcJw4rl4Fs28iOa5e7Pz3tH3dJ/A6ZHJgL7LQVZJsbBwlJ4" +
            "o/5TwTZYTgEuqg/TAqm8LWl4XMERi4WJ2exvz3Wm02v4sDo0c9O/tm293lkVVkmhNAnDGt+8sHIr4/4kM5nPlTQZQ7AB6BIdRyHg" +
            "IB5rRM4gJxMAsmxdbqA7e8DGYWYJoW1oSN7nlBh+1nOW1tt+EHXqIUAojbmzRbpTfKjH1TPPUDPct3bOSaRASoSp8mvpUt5+ba5w" +
            "2Yhfp18VAoHAX6T+pZPLYEOfvx8t3xnrNu12OW5MBBDq3nFsa62b9Yd8BY832P7LilIs57AFzMlPIu1zEmOHyrErXSUID15dlbMW" +
            "wovX81Phy/ewkynVNKLZ1qb9KG6gr0loj/ccTXIqEdE35R6/u3c2+Vv/PlS1MqyO6Qsfjs+1yppgLDn5S+7/f8sehlYhNbBnkEj+" +
            "5r1JvJRYzP+aOpjak+MweLmKUcfiAJQokgiqqRqegP3uox94pKpEf2/Vzm4knl3tz3fBAoHAZURb0o6Rsy1fCoZmsIFyZzsNt6aW" +
            "8twL02e59FrK3/M4L1gZcBS6nUwSmNi48f/wvNqfDkvzIr1NriknmKeFfWV6izbNRclU6maN9mxoY+NpJS2DDufJ0RVZ6X1unWf4" +
            "RL8XHdNb36nsDdQEES+OTWRk8pNTbzJdOoHgg0hdQwUiLGfYlf0w/hVatQJm/xE1emCoR0p4Zp5fztZpwsbspMxPGZW/bKqgRJFF" +
            "+xOaP9aI8Vp2V2V26bEUvXQlJG1JAoHAIPgFZmsmyWx+YhyYgk5HloIa6hQKs5VZM/gKNt2QmWEKbWCJ+Pp7UP9gNoqJltAxLfER" +
            "wjG7LkrYFhXpJ0VWUgbyl1mwoVsdoNxMoGdNvbuphQRyao6yYGkbDifQxnhaUtv8cI4prZH/dORBzKoIQCPlMxUSWmhq7k8+pjWJ" +
            "dl/in7mBWc8/uZybe4QEmCsb0pjLuS5hGL+3sSYhV3/DnKXcd+8hkAfMJuxYYi7LcLr5ZsCN0bsVQ5+o99l6whiH";
    private static final String RSA2048_CA =
            "MIIC+TCCAeGgAwIBAgIDCIvlMA0GCSqGSIb3DQEBDAUAMDMxMTAvBgNVBAMMKEdlc2hlciBwdWJsaWMgdGVzdC1vbmx5IFdpLUZp" +
            "IFJTQTIwNDggQ0EwHhcNMjAwMTAxMDAwMDAwWhcNNDAwMTAxMDAwMDAwWjAzMTEwLwYDVQQDDChHZXNoZXIgcHVibGljIHRlc3Qt" +
            "b25seSBXaS1GaSBSU0EyMDQ4IENBMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAwmx6zeAK/9zZPbVNEcIEpftdMa89" +
            "6XXomA23h8lbebdTsSgl1VW3Ow7EYm83y7Gcyt5uHhWpwbGI4cwzPPuyARAKCM/77tkFOLkqypcq2ajwPGBgAi4GEY6HpqwaFCYt" +
            "9kEpoNbr3mFpmcz5bTfUKIvir/TVT6PvSwcZHzU4qsCbRV5lvQoYOVdmU+0da2c89dRenKo0QNsuaC99VQ/26AkutPYbwANO7a8i" +
            "NlYPTcZCtcutrSWjWfcMVqA/aFYOaa5e6x4brs9EZ+MD4I46pLImEYkKP1N4f2lWOgqPoCK8ajahEWE/FzZvN3bvYWdlVdFO8TmX" +
            "Z/GbUlDX8fDX3wIDAQABoxYwFDASBgNVHRMBAf8ECDAGAQH/AgEBMA0GCSqGSIb3DQEBDAUAA4IBAQBnaKxqcNYUKaSK/eHJu2wk" +
            "qBGc/9BPfIwplL9cqrF1SSWEWdmgalefN1HRvlkngoCXpQhLBJsBz+quRP3F/lpSX2+0w2dOoeVn1iLyTDf5mOrsuC3D6k18IYpw" +
            "U9v7jlUg9B2g7+vpjoh+nAdSVWkvORytwIArJkgEocedc0AzRc+E37zY4PlFg0D9hXUUa+TZWhoSgwkW1Oy41Inos0vUuDf+IHRw" +
            "tCmhbvXc1VebrlfxleoZCEVBm7wqQwN4294vXzaSA4eoOLNhcwOlX/oDKFIRbjlxrn5ve3M8IAur4iEdQC22+y1pVJQ6xcLzLs6N" +
            "wwKdTi3J8ZGZtVxb2tRu";
    private static final String RSA2048_CLIENT =
            "MIIC9zCCAd+gAwIBAgIDCIvmMA0GCSqGSIb3DQEBDAUAMDMxMTAvBgNVBAMMKEdlc2hlciBwdWJsaWMgdGVzdC1vbmx5IFdpLUZp" +
            "IFJTQTIwNDggQ0EwHhcNMjAwMTAxMDAwMDAwWhcNNDAwMTAxMDAwMDAwWjA3MTUwMwYDVQQDDCxHZXNoZXIgcHVibGljIHRlc3Qt" +
            "b25seSBXaS1GaSBSU0EyMDQ4IGNsaWVudDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALI5o4QlSjG9R3Gt8lrnvv++" +
            "8CdwabXFOEhvfedNr1ZS8uEu09LdqJTml+cf5tX/uyqATg+vgqK2Ea8wY7VccN4XqDmpgEyPlNvz1mVNOqbkuW/rlVsuggSiYRd4" +
            "r17LqRlZKCty5R8b/ld/M9FdjViDfrgevH/y5XyTJmjRRbI229jQxbygv/Rrx4uTTPji7OArgyDVR6azd5/7QO0cMGZjtM+WuqBg" +
            "dwxxHN2fbLPvPLwO0iIQj/drwqsQWYt+VQHDn14ywzWJSN/vxpr0aSVjOjvQ3pemZpTvPoFWG1P1+0z2YOhTUnnnb6SXn8kfLIbL" +
            "zUIbUsnGFEcVbfOA48kCAwEAAaMQMA4wDAYDVR0TAQH/BAIwADANBgkqhkiG9w0BAQwFAAOCAQEAV/k6DefLPvtRTr6brWnTcJev" +
            "tEPdKyGRoiF1Vv3rLONoZ3Lgi+g+czMst//zP7rBS10WyF5C2JSb0k+vc2CPyiWzTp2oV9OMbC40Jd4EC7hNeYvirLS1JuZnJhPN" +
            "SN+MZ0ibsmhrVUwwPO0CY6MT0KcRVGMrgQJ5Tpp8c1K0VjJ8MI/etoZY75+zp0Mc6pDRhCGUmSngrNFUunzZh6ETzWrHBPNQY9uF" +
            "uP/JIYcAttdIkr6q56YDVYAzFkcdNLL6u54bKBFiEmPPFchQ56mJ4NPtIiqwM3Rdi4FDMlnm+VmJctIPFEx9Qo5lpNDl3vEYpiOn" +
            "9xCi/UqoAc2OzMDgrw==";
    private static final String RSA2048_KEY =
            "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQCyOaOEJUoxvUdxrfJa577/vvAncGm1xThIb33nTa9WUvLhLtPS" +
            "3aiU5pfnH+bV/7sqgE4Pr4KithGvMGO1XHDeF6g5qYBMj5Tb89ZlTTqm5Llv65VbLoIEomEXeK9ey6kZWSgrcuUfG/5XfzPRXY1Y" +
            "g364Hrx/8uV8kyZo0UWyNtvY0MW8oL/0a8eLk0z44uzgK4Mg1Uems3ef+0DtHDBmY7TPlrqgYHcMcRzdn2yz7zy8DtIiEI/3a8Kr" +
            "EFmLflUBw59eMsM1iUjf78aa9GklYzo70N6XpmaU7z6BVhtT9ftM9mDoU1J552+kl5/JHyyGy81CG1LJxhRHFW3zgOPJAgMBAAEC" +
            "ggEAB7MfHK8SZ4Jf5coNYF6xYmcjWkYsVaKxmHTd9vI6PaZ+a7q+2ACk2dn50g26G/YHu1b+XbByxQjMoXRGZSgbOt0WyCGfM1o4" +
            "pNACAS+29aLdXWJ2JLxQ+FZI5N9dOYve6wNuYtd8jT2DstACCGzcQGizPs0aeQSguXhkqRPxc44slJxUIse40a0ZEifaDFxg8r8e" +
            "TftkcTUK1lkTTmIsE9OfFvz7Iml3fmEdiNtTQ5q1Jg5vtXVD2kL37Qgo8EUTMmcnb6sFPsAe3bo27wqdsyBWEcrI6K4PXHVrmoS3" +
            "5FcIa1O1X+TNhUJ8y/VYTXt7+VoHxDB+/v9nmp/FAjysaQKBgQDxmKg7utbx0thQSesMDHwwehTbJzapsQ7c3kLUUDYuEaAKrfel" +
            "nEuDEl5ybG0++HrSCDrsPUPrie0khH3aE7B0ebMqH5Pe0MXLYN8iFYl6N6ZOPyKVBQGKGjxqefDDgLG/MZBwk4YfZDY72bm+muj8" +
            "L832craBOZCKIU8O80hNawKBgQC82cje9XG3GWLBCzQE/PBpp/dWUdVfCRlb0k4BPyc/ktFDyOLSw+RRPZbc5bITyfv5T5+JrZ4M" +
            "sN8LnX4gbmZOxEAIIK57WZv7jRKDA3/XziYt61016Q4aRwexnaOxiyahR+6V4+KXQseAAOxSm+BXkLzCLNxsV6HdY37qLosMmwKB" +
            "gQDD9HHfoHfexIk29f1zVF0WZK6t4mxvznCjrJK/6h9H1nvcraYD6XVF5ow8er1RKfFUkeBY5IhGbyStTdlWHEou7tbm/53MR8Fm" +
            "aDgTSSysuJ5nMMY34EuTXBxMsIMbzzJcRTNGXCGZLIrgmIQTmUoH4i5IClsFDU+y+blKAwFN6QKBgQCNzNVYHGHTQvt0ye24ZRg7" +
            "rUQ5i160akehO4sYwou834fD5v3xledE3QL/QHVzCHHrpE69eSC4lMIVnZ8LyPmG4TLXJ3r74bAOtMGWoDUJHKBsUFL/3MubDYSI" +
            "aD52XG6YIUPTRnZob1sPESJhcOuFtIcXIcosi6xdaIzuWB5OSwKBgC8//o73q9B8exypS63pRxtenwgr8cG7DufuBnhWbo6vuxwa" +
            "FJSqghY+Ta1TO3C2exjhBt8XbHzE6jON3rtiypTeZUPMRKEnJ0H1EIcUJK7XHAePOuTfWCVO9ayIKnCQCV8fF1dhA7+5+l5hBGJ+" +
            "XZCm82o07CACyFtitSBK1efN";
    private static final String RSA3072SHA256_CA =
            "MIIEBTCCAm2gAwIBAgIDCIvlMA0GCSqGSIb3DQEBCwUAMDkxNzA1BgNVBAMMLkdlc2hlciBwdWJsaWMgdGVzdC1vbmx5IFdpLUZp" +
            "IFJTQTMwNzJTSEEyNTYgQ0EwHhcNMjAwMTAxMDAwMDAwWhcNNDAwMTAxMDAwMDAwWjA5MTcwNQYDVQQDDC5HZXNoZXIgcHVibGlj" +
            "IHRlc3Qtb25seSBXaS1GaSBSU0EzMDcyU0hBMjU2IENBMIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAx1DC6rrxS6EN" +
            "j5AAPgtvfXj2uXBrQWy/OFWrhs9auQQb5Qi+Aarj9vIwaDolPdTI0kt3hjyZGmGcolBXNBESKGiWxQyMyy0rHCTGcnJXyUPAbSk4" +
            "W9GnlJk0LQcv7qj4fer8E44MIG8FVxzN5RfpVFp8QbZZ+6pdVuvkw096PwTU95q5EnrpKRwIaMuDB4uNLy7WOLZZy/D2BtWSE1wZ" +
            "bEM/JP6O3t/BVZWJh5O+QMOrPy00lpIiyR/SV3CMv8QxMiQ3GNbaNLTSN5dhsYPSamHUHUGt5oGWdSa0Un4+mT+WWTUzxE81IqJR" +
            "tWHh24629Wgc3cc70q2EYil9C799CakrbVm7wOPrwCNrMVplSLjfB+obJuhtfL3KvHyth8tsqy6znjn3ADUBLaGQtg1GQWt5qLPx" +
            "Yv8OlsWT3xKEelimbUFxNz9nAhtC+jUKV6To+kwmSYQFcK/4peZtqdzGNuSHQ7JGu1tImssK37T0mQRJYsBqSyJDMM1BfqZHlqlR" +
            "AgMBAAGjFjAUMBIGA1UdEwEB/wQIMAYBAf8CAQEwDQYJKoZIhvcNAQELBQADggGBAIDllN6hXtdzoEOqpKe1eLI9BSBSj6QNRiUl" +
            "Wxpc2aKPtQaKsVn+VpDxUIoUjSWDIIZ1KI120D6XrJ4G9KQj939tho4rHc0GKrMxEhU6bQtT/8HeL1g76wwZX6c9dTXRXFvtVzZl" +
            "EWEXNaE5kh9lyUlCHodXv5FCVNs/eozEDXbadH6F2sRXg8DZozQZB+YycOggZGil4/ZTeyRqyCUP1m/gpXQH9bU0pkxA90ESO3ZG" +
            "xZByZdJp8ViyIpHmsvrg7DcqkN+I/7tWfA7WNX9fSydSKeqAlahnyaGSuVVEnylnwXExemBLzfd/waIH+v7PjuwMtO+dVlX/ogOb" +
            "2/BBkPe38GpV/FyaRnX1JYAnS6QZ4j3mJjtG2BTcoH+S509frDoeStVBt3iO18Hd9Yk/nnNKP1nAuBtvV/T9MPHwd/vdgw8uMBwp" +
            "Gkzb2OyvTwz3Wza9MLGaUsTL2A6KrDBB+i8NwggjH4FrgDvnE+x0ftbXW19B4QhK7mxNNJKUNW6IUA==";
    private static final String RSA3072SHA256_CLIENT =
            "MIIEAzCCAmugAwIBAgIDCIvmMA0GCSqGSIb3DQEBCwUAMDkxNzA1BgNVBAMMLkdlc2hlciBwdWJsaWMgdGVzdC1vbmx5IFdpLUZp" +
            "IFJTQTMwNzJTSEEyNTYgQ0EwHhcNMjAwMTAxMDAwMDAwWhcNNDAwMTAxMDAwMDAwWjA9MTswOQYDVQQDDDJHZXNoZXIgcHVibGlj" +
            "IHRlc3Qtb25seSBXaS1GaSBSU0EzMDcyU0hBMjU2IGNsaWVudDCCAaIwDQYJKoZIhvcNAQEBBQADggGPADCCAYoCggGBAI2INApF" +
            "V8BSUdurpGNwuwWHnOTRoYKc9I3SImrzzlGyF3f9keEDqF7qt+C2br0FfDCdlfG96XPXSCVoGtBK76S+Kp8ildY9hyhztn8jzj34" +
            "bKl/1umSgPe1pgcmugwNzh+ESBybnKi0UewMJhKUMLuKxNlqLVtH0kkKx5Oa08/6ix5EvcTtftswWob7+R8BS8d/gM9ev7lZF7w+" +
            "gFzAN2XQIX6z7WPs01TYJZ6qQHlP7eh+3OjmMtPvQ7aDeFmuJ2WqwIH02oUV3iB9p8KBFe4ZIEBvqdkn+yMPWD9BTvPXXOVzXr7c" +
            "+wY70ZONJr9BjG8ar1i4EvgGkicjqTg8MOxjp/nLPXEMtR9uU/vmeaOy5BUxieDHMUhW6+fzgogbIaJRDb0O+ltPuKrSLIFrZdtr" +
            "tWNbJNiSCoDGWT+xydMKCw6OozcHCD84aVb9z4VPfqrY/Ey+VZ7O+GH1TWn0fcuvPLGyTmM8NL1ET1V88jkUYPuI7lx/ozEobwGU" +
            "TDnq+QIDAQABoxAwDjAMBgNVHRMBAf8EAjAAMA0GCSqGSIb3DQEBCwUAA4IBgQCV/m6RDP4EiJLd2/EaFemwRVa1Ghx1dlzei8ol" +
            "u8fCBAv/iAzgATG9+P7641e/0a7ZqhciyYt5f6xtNXbGu3vpnhvsyAndX6aN3k/pLIBl8eZE7ZzujgvkuEehQNR82FlsR7lh/w+i" +
            "qpefFchIkiZ0nDwIdcgdcVYKLcZRLQ9jOLUBz3Zuv9RlAVOg+nG5mygUh0V8mJMrd8HKt2KIkN469pCIlIgmb3N9l8VCDUy4YOC2" +
            "b58z1uA1aL6bqdGbeADIsBEJ7bV6ywxYM5M8B/J6TJJqfmG9ve/z/t7t65WAS6cNXxYIUehn9yYg433gGWq7DhEsosPknnLunHDe" +
            "aNk+i6ljfiUO3Hu51J2aK3HJ9TktxkVaIQWiaemR5GN3cjGaCHKPlAy1iGK2N7ZLRXoogxA104O/yTsxFtZWVT4QwbwScUTJWZQx" +
            "T4OuaYRvNcF+I7VUmLAvRbNa6TUp9Bjdv/jica17W0f7iVUyU04CHZqERRT++8m6jcVwmv3r47M=";
    private static final String RSA3072SHA256_KEY =
            "MIIG/QIBADANBgkqhkiG9w0BAQEFAASCBucwggbjAgEAAoIBgQCNiDQKRVfAUlHbq6RjcLsFh5zk0aGCnPSN0iJq885Rshd3/ZHh" +
            "A6he6rfgtm69BXwwnZXxvelz10glaBrQSu+kviqfIpXWPYcoc7Z/I849+Gypf9bpkoD3taYHJroMDc4fhEgcm5yotFHsDCYSlDC7" +
            "isTZai1bR9JJCseTmtPP+oseRL3E7X7bMFqG+/kfAUvHf4DPXr+5WRe8PoBcwDdl0CF+s+1j7NNU2CWeqkB5T+3oftzo5jLT70O2" +
            "g3hZridlqsCB9NqFFd4gfafCgRXuGSBAb6nZJ/sjD1g/QU7z11zlc16+3PsGO9GTjSa/QYxvGq9YuBL4BpInI6k4PDDsY6f5yz1x" +
            "DLUfblP75nmjsuQVMYngxzFIVuvn84KIGyGiUQ29DvpbT7iq0iyBa2Xba7VjWyTYkgqAxlk/scnTCgsOjqM3Bwg/OGlW/c+FT36q" +
            "2PxMvlWezvhh9U1p9H3Lrzyxsk5jPDS9RE9VfPI5FGD7iO5cf6MxKG8BlEw56vkCAwEAAQKCAYABPSb6IV9CRLYzY7joUe1L1W25" +
            "0bKbOmp9mfrz0AyHSeAgtxcsJA8OPaRnEtG8qoFKylECBl/hGV+n9tWn6xqr6NTmAwnSupkl8SqwyYxy8xYTFGu6LFkf/KFma0qy" +
            "SPtRP/mC8+3BdZ/zSVaapQdu13509L6kOj23UUvWmUKvYwm4Cxv/eaC5pHO2aMWRJZQXO/YnBTT3wgrjepyUXe5eNXMwRmeWj1Uj" +
            "kfzKUG0zYqVkz+95GRgVeNZb5ky1uZYwB+++3jiYdqTfY4oycOJzOaTY9F5oJ5Udfo3wGnYXNvt/FAVQj1/UY4w9d+dFtBbKA8w/" +
            "fAb4QgYBEKQ1pACeuKDW7sFl+ciYUjM6wuYuclQXnoYko6JN+UeYkeAMsU6BGO3oUZ1POTs7RuvM1gZL7GCeikAllideSFYc/SiK" +
            "hqoM9eCIH5xivlZr5G6EhMjoDW/pWhRY/vOLDN6+diuxY58LCyd3HuyGT9TszgJRu3P69z2FLqbhouuBMqAvtm0CgcEAwzgEdENk" +
            "r6vuJdhfn4sjsElJ6gViB7KpXZLsVEXtM7FY3d2Vw0l/c4B9MBXnxCq1SE/0KS8nJc5VrOV5GCbCHgegkh5+YWRITu9LO1N2fccp" +
            "eSuj8pGezyMzHvm8Um3sd4ggMXTO6NIi7j1QicebHBIutdd7gcQ2v5oW6VWLwlQmob91WJHtxvgTHLwC/9bo7lhvAUAhUqGlJxDn" +
            "1G0CSfdvHcmkGfIr4e3AN1HvgnVhBrhH7K7kiK2YPkcmZVALAoHBALmZEC9TZEXpRE5bY4Po56GU5kjIR0mBP3DYdEB79vGASZ4w" +
            "A1TRClSu2fWfrFbbHs2TuZJ64nisvWeokizIHWn9npU5dy+O4fu5sZn9ixnLD5HIH7jCKqv2etK7Ue5EGhAdHl+i9pByE6Z/ja8g" +
            "rvRNPDDFQGTNGoOSuHLysL6CKs2HjybV3JR9Gi2twftxs48g69iDP0V/pCZUM06c+3riTTTq94OTFKbRnnPguok0eUx6wLnRA4zA" +
            "ikCj2MZ/iwKBwQC2HYH6PAyn3UqTP6ElIxYuFJDBojfrWn6pbrDNhVZdTOrMPzTBM8WUKJ+gxi6qT/VB+mSaLwTkPfKrEn+/Shj5" +
            "DsB6z7uye20BRjb2p0v41l0YTt/pIHP4ZKwCD5Phie55LCJsQL3tcGtpuQVzIyTOMSAmP1lWm4c/gy4wir+QkBRh/EwqTih132Nl" +
            "APgDV6eBAjq7/emYPnflU94Ry4fBHPUp4rwWAFdzsuoIMYWwPHRBYpSmdhl4cAmAv7tP9DUCgcB4nDR9XMbz+7QM66zPAyLLyXSh" +
            "Za2V4e01SyS45xZCOKkeqHrxumrpPzubOCmJxBnPZKmavErCOxdW7qAXg/BBebR9qvt+J29J45G7zZ2ApIo4pYGf0mZim+mTWnHS" +
            "0eTSBepAIinzEn8cmxWa8jVfw3VTCExhdTw+trEw2e/rzbE9IVPw29fdkzLY7mr6GRij8bPyXtBWPK2SzvMMqk1uvPOJaQxyL7mi" +
            "8uPyz97I3RXakob9+gtBg7oUaaWs0SMCgcAcSYQpUy3HA06fbZBI0r7rBb/gdLmXnIbaHrkWvWs+cU753eERpnaCvcjEqonLeq72" +
            "0Rdm7L8eV/r4cCRY9qVAhXnF+G2jhn7i76Xi7tvxL/ygiqCwhzjXW50DQCVfIJUjCzBox+6XNp2z1to8pudezAPlFP6wKXmSu8qc" +
            "RfqUgGP71XAdbLdgnDWYqRc2+JQ2vYui0CNHjgeSlRL0ecD3Nu+40w+CFY5wrWRng2BNx9f91z7nWq0X8Qy320f1xII=";
    private static byte[] bytes(String value) { return Base64.getDecoder().decode(value); }
    private static X509Certificate certificate(String value) throws Exception {
        return (X509Certificate) CertificateFactory.getInstance("X.509").generateCertificate(
                new ByteArrayInputStream(bytes(value)));
    }
    static WifiEnterpriseConfig config(String kind) throws Exception {
        String ca, client, key, algorithm;
        switch(kind) {
            case "EC384": ca=EC384_CA; client=EC384_CLIENT; key=EC384_KEY; algorithm="EC"; break;
            case "RSA3072": ca=RSA3072_CA; client=RSA3072_CLIENT; key=RSA3072_KEY; algorithm="RSA"; break;
            case "RSA2048": ca=RSA2048_CA; client=RSA2048_CLIENT; key=RSA2048_KEY; algorithm="RSA"; break;
            case "RSA3072SHA256": ca=RSA3072SHA256_CA; client=RSA3072SHA256_CLIENT; key=RSA3072SHA256_KEY; algorithm="RSA"; break;
            default: throw new IllegalArgumentException(kind);
        }
        PrivateKey parsed=KeyFactory.getInstance(algorithm).generatePrivate(new PKCS8EncodedKeySpec(bytes(key)));
        WifiEnterpriseConfig result=new WifiEnterpriseConfig();
        result.setEapMethod(WifiEnterpriseConfig.Eap.TLS);
        result.setCaCertificate(certificate(ca));
        result.setClientKeyEntryWithCertificateChain(parsed,new X509Certificate[]{certificate(client),certificate(ca)});
        return result;
    }
}
