# ReadMe

CF enhanced blockchain, and aims to build a platform to provide common blockchain smart contract services. 

As a fledgling technology, existing blockchain implementations have fallen short of meeting the multitude of requirements inherent in the complex world of business transactions. Scalability challenges, and the lack of support for confidential and private transactions, among other limitations, make its use unworkable for many business critical applications. In order for the platform to be resilient to time and support requirements across the industries, it needs to be lightweight, modular and support extensibility through configuration and plug ability of various components (transaction validators, block consensus etc.).

CF derives from IBM fabric (https://github.com/CFQuantum/fabric).

### Repository Contents
#### ./bin
Scripts and data files for cfqd integrators.

#### ./build
Intermediate and final build outputs.

#### ./Builds
Platform or IDE-specific project files.

#### ./doc
Documentation and example configuration files.

#### ./src
Source code directory. Some of the directories contained here are
external repositories inlined via git-subtree, see the corresponding
README for more details.

### API SDK apply
For creating 3rd party software or APPs, RPC call is needed by those developers. We provide a simple client API SDK to support it. 

#### 1. User info
##### URL：`https://www.cfquantum.org/api/user/balance`
##### Method: GET
##### Parameters：`None`


#### 2. Send activation Email
##### URL: `https://www.cfquantum.org/api/user/send_activated_mail`
##### Method: GET
##### Params: 
Parameter|optional|Description|Example
-----|-----|-----|---
email|YES|activate Email|email=test@gmail.com

#### 3. Get currencies user hold
##### URL: `https://www.cfquantum.org/api/user/currency`
##### Method: GET
##### Params: `None`


#### 4. Payment
##### URL: `https://www.cfquantum.org/api/tx/auth2/make`
##### Method: `POST`
##### Description:
 You may pay directly, while sending from your mobile. And every 20 times, you have to be verifid by Google two-factor authentication.
##### Parameters:
Parameter|Optional|content|type|Description
----|----|----|----|----
json|true|recipient_addr|String|account address or nickname
|||selectedCurrency|String|currencyCode
|||amount|String|amount of payment 
|||unlock_password|String|None 
|||unlock_password_pay|String|Googleauth code

#### 5. Historical transactions
##### URL: `https://www.cfquantum.org/api/tx/account_tx`
##### Method: `GET`
##### Description:
 List all transactions
##### Parameters:
Parameter|Optional|Description|Example
----|----|----|----
maker|YES|set null|maker=null

#### 6. Get book offers
##### URL: `https://www.cfquantum.org/api/tx/book_offers`
##### Method： `GET`
##### Parameters: 
Parameter|Optional|Description
----|----|----
currency1|YES|currency code
issuer1|YES|set "" if currency is CFQ
currency2|YES|currency code
issuer2|YES|issuer of USD is<br>"chaQfQJHfCWaCgRdx327RhZoJuaK5q87kS"<br>issuer of CNY is<br>"cfqaZrjWtdcXgZZ7zCLztBLs7jsFF5yKR6"
limit|YES|display number


#### 7. Make a offer

##### URL:`https://www.cfquantum.org/api/tx/make_offer?`
##### Method: `POST`
##### Parameters
Parameter|Optional|Description
----|----|----
json|true|
dynamic<br>Password|false|
##### Example:
```
json={
	"first_amount":{
		"currency":"CFQ",// currency code
		"issuer":"cha…..",//gateway account
		"value":"0.050787"//amount
	},
	"buy_amount":{
		"currency":"CFQ",
		"issuer":" cha…..",
		"value":"0.050787"
	},
	"sell_amount":{
		"currency":"USD",
		"issuer":" cha…..",
		"value":"1"
	}
}
dynamicPassword=1234
```

#### 8.withdraw

##### URL:`https://www.cfquantum.org/api/agent/auth2/order`
##### Method:`POST` 
##### Parameters:

----|Parameter|Optional|Type|Description
----|----|----|----|----
json|amount|YES|Double|
||orderType|YES|Integer|2:withdraw
||firstCurrency|YES|String|
||secondCurrency|YES|String|
||payType|YES|String|paypal,card,other
||description|NO|String|
dynamic<br>Password||false|String|


#### ......


Apply SDK and key for RPC by email: cs@cfquantum.org with title "SDK apply ...". Describe your APP and estimated frequency of RPC in this email.

### License
CF is open source and permissively licensed under the ISC license. See the LICENSE file for more details.
